// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Tier A: async/poll completion + in-flight pin lifetime.
//
// Tests in this file (mapping kept for tests/README.md cross-ref):
//   - TessaiAgentFence.PollOnSyncFile               (sync_file fd poll)
//   - TessaiAgentFence.FenceStatusBeforeAndAfter    (SYNC_IOC_FILE_INFO)
//   - TessaiAgentFence.BusyPollSleepInterval        (10 ms busy poll)
//   - TessaiAgentFence.InflightPinSurvivesGemClose  (early GEM_CLOSE)

#include "common.h"

#include <array>
#include <cstdio>

using namespace tessai_test;

class TessaiAgentFence : public TessaiAgentTest {};

// ---- completion observable via poll(2) on sync_file fd -------------

// Export the syncobj as a one-shot sync_file fd, poll(POLLIN, 2s),
// assert the fd becomes readable when the firmware signals. Documents
// lib-userspace-flow.md §2.1.
TEST_F(TessaiAgentFence, PollOnSyncFile)
{
	uint32_t syncobj = 0;
	ASSERT_EQ(do_cmd_submit(fd_, 0xA0010001u, nullptr, 0, &syncobj), 0);
	ASSERT_NE(syncobj, 0u);

	int sync_fd = -1;
	ASSERT_EQ(do_syncobj_to_fence_fd(fd_, syncobj, &sync_fd), 0);
	ASSERT_GE(sync_fd, 0);

	struct pollfd pfd{};
	pfd.fd = sync_fd;
	pfd.events = POLLIN;
	int pr = ::poll(&pfd, 1, 2000);
	EXPECT_GT(pr, 0) << "poll timed out: firmware didn't signal in 2s";
	EXPECT_NE(pfd.revents & POLLIN, 0)
		<< "revents=0x" << std::hex << pfd.revents;

	int32_t status = 0;
	EXPECT_EQ(do_syncobj_fence_status(fd_, syncobj, &status), 0);
	EXPECT_EQ(status, 1)
		<< "post-poll fence_status should be signaled-ok";

	::close(sync_fd);
	(void)do_syncobj_destroy(fd_, syncobj);
}

// ---- SYNC_IOC_FILE_INFO probe before+after blocking wait -----------

// The "before" probe is intentionally racy: a fast firmware may have
// already signaled by the time the second syscall lands. We accept
// either 0 (pending) or 1 (already signaled). The "after" probe is
// strict: MUST be signaled-ok.
TEST_F(TessaiAgentFence, FenceStatusBeforeAndAfter)
{
	uint32_t syncobj = 0;
	ASSERT_EQ(do_cmd_submit(fd_, 0xA0020002u, nullptr, 0, &syncobj), 0);
	ASSERT_NE(syncobj, 0u);

	int32_t st_before = -1;
	ASSERT_EQ(do_syncobj_fence_status(fd_, syncobj, &st_before), 0);
	EXPECT_TRUE(st_before == 0 || st_before == 1)
		<< "fence_status before wait: " << st_before
		<< " (expected 0 pending or 1 signaled)";

	ASSERT_EQ(do_syncobj_wait(fd_, syncobj, kSubmitTimeoutNs), 0);

	int32_t st_after = -1;
	ASSERT_EQ(do_syncobj_fence_status(fd_, syncobj, &st_after), 0);
	EXPECT_EQ(st_after, 1)
		<< "fence_status after wait should be signaled-ok";

	(void)do_syncobj_destroy(fd_, syncobj);
}

// ---- busy-poll with 10ms sleep ------------------------------------

TEST_F(TessaiAgentFence, BusyPollSleepInterval)
{
	uint32_t syncobj = 0;
	ASSERT_EQ(do_cmd_submit(fd_, 0xA0030003u, nullptr, 0, &syncobj), 0);
	ASSERT_NE(syncobj, 0u);

	int32_t status = 0;
	uint64_t t0 = now_ns();
	int rc = do_syncobj_busy_poll(fd_, syncobj, kSubmitTimeoutNs,
				      10ull * 1'000'000ull, &status);
	uint64_t dt = now_ns() - t0;

	EXPECT_EQ(rc, 0) << "busy_poll returned " << rc;
	EXPECT_EQ(status, 1);
	std::cout << "  busy_poll(interval=10ms) elapsed "
		  << dt / 1.0e6 << " ms" << std::endl;

	(void)do_syncobj_destroy(fd_, syncobj);
}

// ---- in-flight pin survives early GEM_CLOSE -----------------------

// alloc -> mmap -> TO_DEVICE sync -> submit with BO in args[0] ->
// GEM_CLOSE IMMEDIATELY -> wait -> verify writeback observable via
// the surviving mmap. The mmap holds its own page reference (taken
// by the VMA when mmap was issued) so it outlives the GEM handle.
// The driver's bo_refs[] pin (taken during submit's ioctl handling)
// keeps the underlying dma_alloc pages allocated past close. Documents
// buffer-management.md §4 end-to-end.
//
// Lifetime contract for the post-close half (do_syncobj_wait ->
// uspace_dcache_invalidate -> sscanf):
//   - do_gem_close ONLY drops the BO handle from the per-fd idr;
//     fd_ itself stays open for the rest of the test. This is the
//     whole reason the in-flight pin is observable: subsequent
//     ioctls on the same fd (e.g. DRM_IOCTL_SYNCOBJ_WAIT) continue
//     to work. Even hypothetically -- if fd_ were also closed --
//     none of the lines below crash: the wait is an ioctl that
//     fails with -EBADF (caught by the EXPECT_EQ + early return
//     below), and uspace_dcache_invalidate + sscanf operate on the
//     mmap pointer `map`, which doesn't touch fd_ at all and stays
//     valid past fd close because the VMA holds its own page ref
//     (mmap'd memory survives the file descriptor that minted it,
//     by POSIX contract).
//   - Post-wait we must invalidate userspace caches manually (the
//     GEM handle is gone so TESSAI_BUF_SYNC is unreachable) -- arm64
//     DC CIVAC over the mmap'd page, exactly what the kernel would
//     have done on our behalf if we still had a handle.
//
// The EXPECT_EQ + explicit early-return pattern (vs ASSERT_EQ) here
// gives a richer failure message ("wait failed rc=N errno=M after
// GEM_CLOSE; mmap still valid but writeback unverifiable") AND
// guarantees munmap runs even on the wait-failure path, so a
// regression that breaks the in-flight pin doesn't also leak a
// mapping or starve subsequent tests of the per-fd xa slot.
TEST_F(TessaiAgentFence, InflightPinSurvivesGemClose)
{
	ASSERT_GE(fd_, 0) << "fixture invariant: fd_ must be open";

	uint32_t bo = 0;
	ASSERT_EQ(do_buf_alloc(fd_, TESSAI_MEMORY_RPU, kFwRspBoSize, &bo), 0);

	uint64_t off = 0;
	ASSERT_EQ(do_map_dumb(fd_, bo, &off), 0);

	void *map = ::mmap(nullptr, kFwRspBoSize, PROT_READ | PROT_WRITE,
			   MAP_SHARED, fd_, static_cast<off_t>(off));
	ASSERT_NE(map, MAP_FAILED);
	std::memset(map, 0x0a, kFwRspBoSize);

	ASSERT_EQ(do_buf_sync(fd_, bo, TESSAI_BUF_SYNC_TO_DEVICE, 0, 0), 0);

	std::array<struct tessai_arg, 3> args{};
	args[0].size = kFwRspTextMax;
	args[0].bo.drm_buffer_handle = static_cast<int32_t>(bo);
	args[0].bo.offset = 0;
	args[1].size  = 0;
	args[1].value = 0xA0044001ull;
	args[2].size  = 0;
	args[2].value = 0xA0044002ull;

	uint32_t syncobj = 0;
	ASSERT_EQ(do_cmd_submit(fd_, kFwEchoOp, args.data(),
				static_cast<uint32_t>(args.size()), &syncobj),
		  0);

	// Drop the handle now (before the firmware has even responded).
	// In-flight pin keeps the BO alive. fd_ stays open -- see the
	// lifetime contract in the comment above this function.
	ASSERT_EQ(do_gem_close(fd_, bo), 0);

	// Defence in depth: re-assert fd_ before the wait so a future
	// refactor that accidentally closes fd_ before this point fails
	// loudly with the exact reason, not as a cryptic EBADF that's
	// hard to attribute to the wrong line.
	ASSERT_GE(fd_, 0)
		<< "fd_ closed between submit and wait; in-flight-pin test "
		   "cannot proceed (post-close ioctls would fail EBADF)";

	int wait_rc = do_syncobj_wait(fd_, syncobj, kSubmitTimeoutNs);
	SCOPED_TRACE(testing::Message()
		     << "do_syncobj_wait rc=" << wait_rc
		     << " errno=" << errno
		     << " (BO handle was GEM_CLOSE'd before this wait; "
			"mmap=" << map << " survives by VMA page ref)");
	if (wait_rc != 0) {
		// Wait failed -- e.g. fence errored, timed out, or fd_
		// went bad. Don't deref `map` for the writeback banner:
		// even though the mapping is technically still valid
		// (mmap outlives fd), the firmware never signalled, so
		// the BO contents are whatever sat there pre-submit
		// (our 0x0a memset) and the sscanf parse would fail
		// noisily without adding signal. Skip straight to
		// cleanup so we surface the wait error and not a
		// downstream sscanf-mismatch red herring.
		ADD_FAILURE() << "wait failed; skipping writeback check";
		(void)do_syncobj_destroy(fd_, syncobj);
		::munmap(map, kFwRspBoSize);
		return;
	}

	// Wait succeeded -> the RPU completed the writeback. Pull the
	// freshly-written bytes into the dcache via DC CIVAC; without
	// this the CPU may still see the pre-submit 0x0a fill from its
	// L1 because TESSAI_BUF_SYNC is unreachable (no handle).
	uspace_dcache_invalidate(map, kFwRspBoSize);

	unsigned int rpu_cid = 0, rpu_nargs = 0;
	int matched = std::sscanf(static_cast<const char *>(map),
				  "rsp: command id: %u, num_args: %u",
				  &rpu_cid, &rpu_nargs);
	if (matched != 2) {
		std::cerr << "  inflight pin failed; first 64 bytes:";
		for (int i = 0; i < 64; ++i) {
			if (i % 16 == 0) std::cerr << "\n  ";
			std::fprintf(stderr, "%02x ",
				     static_cast<const uint8_t *>(map)[i]);
		}
		std::cerr << std::endl;
	}
	EXPECT_EQ(matched, 2);
	EXPECT_EQ(rpu_nargs, 3u);

	(void)do_syncobj_destroy(fd_, syncobj);
	::munmap(map, kFwRspBoSize);
}
