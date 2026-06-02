// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Pure negative-API tests. New coverage gained when this gtest tree
// replaced the older C smoke binary (which only walked the happy
// path). Mirrors test-error.cpp in the hip suite (both happy +
// negative + multi-threaded coverage).
//
// Anything that probes an error path WITHOUT needing a working
// firmware lives here; fault-injection (REMI/NOTS/CANC/BADC) is in
// test-fault.cpp because those require the RPU fault opcodes to be
// recognized by the slave.

#include "common.h"

#include <atomic>
#include <thread>

using namespace tessai_test;

class TessaiAgentError : public TessaiAgentTest {};

// ---- TESSAI_BUF_ALLOC negative cases ------------------------------

TEST_F(TessaiAgentError, BufAllocInvalidMemoryType)
{
	// Driver should reject memory_type values outside TESSAI_MEMORY_*.
	struct tessai_buf_alloc a {};
	a.memory_type = 0xDEADBEEFu;
	a.size = 4096;
	EXPECT_LT(::ioctl(fd_, TESSAI_BUF_ALLOC, &a), 0);
	EXPECT_EQ(errno, EINVAL);
}

TEST_F(TessaiAgentError, BufAllocZeroSize)
{
	// size=0 has no defensible meaning; driver caps at >0.
	struct tessai_buf_alloc a {};
	a.memory_type = TESSAI_MEMORY_RPU;
	a.size = 0;
	EXPECT_LT(::ioctl(fd_, TESSAI_BUF_ALLOC, &a), 0);
	EXPECT_EQ(errno, EINVAL);
}

// ---- TESSAI_BUF_SYNC negative cases -------------------------------

TEST_F(TessaiAgentError, BufSyncInvalidHandle)
{
	// Bogus GEM handle should produce ENOENT (or similar non-zero
	// errno -- the assertion is just "fails cleanly, doesn't crash
	// the kernel").
	struct tessai_buf_sync s {};
	s.drm_handle = 0xDEADu;
	s.direction  = TESSAI_BUF_SYNC_TO_DEVICE;
	s.offset     = 0;
	s.length     = 4096;
	EXPECT_LT(::ioctl(fd_, TESSAI_BUF_SYNC, &s), 0);
}

TEST_F(TessaiAgentError, BufSyncInvalidDirection)
{
	// Allocate a real BO, then pass an invalid direction.
	uint32_t bo = 0;
	ASSERT_EQ(do_buf_alloc(fd_, TESSAI_MEMORY_RPU, kFwRspBoSize, &bo), 0);

	struct tessai_buf_sync s {};
	s.drm_handle = bo;
	s.direction  = 0xFFu;  // not TO_DEVICE/FROM_DEVICE
	s.offset     = 0;
	s.length     = 0;
	EXPECT_LT(::ioctl(fd_, TESSAI_BUF_SYNC, &s), 0);
	EXPECT_EQ(errno, EINVAL);

	EXPECT_EQ(do_gem_close(fd_, bo), 0);
}

TEST_F(TessaiAgentError, BufSyncOffsetBeyondEnd)
{
	uint32_t bo = 0;
	ASSERT_EQ(do_buf_alloc(fd_, TESSAI_MEMORY_RPU, kFwRspBoSize, &bo), 0);

	struct tessai_buf_sync s {};
	s.drm_handle = bo;
	s.direction  = TESSAI_BUF_SYNC_TO_DEVICE;
	s.offset     = kFwRspBoSize + 1;
	s.length     = 1;
	EXPECT_LT(::ioctl(fd_, TESSAI_BUF_SYNC, &s), 0);

	EXPECT_EQ(do_gem_close(fd_, bo), 0);
}

// ---- GEM_CLOSE negative case --------------------------------------

TEST_F(TessaiAgentError, GemCloseInvalidHandle)
{
	struct drm_gem_close c {};
	c.handle = 0xDEADu;
	EXPECT_LT(::ioctl(fd_, DRM_IOCTL_GEM_CLOSE, &c), 0);
	EXPECT_EQ(errno, EINVAL);
}

// ---- TESSAI_CMD_SUBMIT negative case ------------------------------

// num_args claims 2 but we only allocate enough for 0. tessai_cmd's
// copy_from_user should EFAULT out of bounds without dereferencing
// the missing tail. This is the "userspace lies about size" canary.
TEST_F(TessaiAgentError, CmdSubmitLiesAboutNumArgsTrailingBytes)
{
	// Hand-rolled buffer: 1 struct + zero args, but num_args lies.
	struct tessai_cmd_submit hdr {};
	hdr.command_op = 0xCAFEBABEu;
	hdr.num_args   = 2;
	// Don't allocate the trailing args. The driver's
	// copy_from_user(args, user_ptr, num_args * sizeof(arg)) on a
	// short userspace buffer should EFAULT cleanly.
	int rc = ::ioctl(fd_, TESSAI_CMD_SUBMIT, &hdr);
	// We don't assert which errno -- different kernels return
	// EFAULT vs EINVAL -- only that the call fails.
	EXPECT_LT(rc, 0);
}

// ---- syncobj negative cases ---------------------------------------

TEST_F(TessaiAgentError, SyncobjWaitOnInvalidHandle)
{
	uint64_t handles64 = 0xDEADu;
	struct drm_syncobj_wait w {};
	w.handles        = reinterpret_cast<uint64_t>(&handles64);
	w.timeout_nsec   = static_cast<int64_t>(now_ns() + 100'000'000ull);
	w.count_handles  = 1;
	w.flags          = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
	EXPECT_LT(::ioctl(fd_, DRM_IOCTL_SYNCOBJ_WAIT, &w), 0);
}

TEST_F(TessaiAgentError, SyncobjDestroyTwiceFails)
{
	// First submit -> get a real handle -> destroy it -> destroy
	// again should fail. Validates the syncobj idr is actually
	// freed on first destroy.
	uint32_t syncobj = 0;
	ASSERT_EQ(do_cmd_submit(fd_, 0xBAADF00Du, nullptr, 0, &syncobj), 0);
	ASSERT_EQ(do_syncobj_wait(fd_, syncobj, kSubmitTimeoutNs), 0);
	EXPECT_EQ(do_syncobj_destroy(fd_, syncobj), 0);
	EXPECT_LT(do_syncobj_destroy(fd_, syncobj), 0);
}

// ---- open negative ------------------------------------------------

TEST(TessaiAgentErrorNegative, OpenNonexistentDriPath)
{
	// /dev/dri/card99 doesn't exist on any tessai-agent host.
	int fd = ::open("/dev/dri/card99", O_RDWR | O_CLOEXEC);
	EXPECT_LT(fd, 0);
	if (fd >= 0)
		::close(fd);
}

// ---- multi-threaded variant ---------------------------------------

// Pound BUF_ALLOC + GEM_CLOSE in a tight loop across threads. Catches
// races in the per-file xa allocation / freeing paths (we've seen
// "BO leaked after GEM_CLOSE" regressions in this kind of stress).
TEST_F(TessaiAgentError, BufAllocCloseStressMultiThreaded)
{
	constexpr int kNumThreads = 4;
	constexpr int kCyclesPerThread = 64;
	std::atomic<int> fails{0};
	std::array<std::thread, kNumThreads> threads;
	for (int t = 0; t < kNumThreads; ++t) {
		threads[t] = std::thread([&] {
			for (int i = 0; i < kCyclesPerThread; ++i) {
				uint32_t bo = 0;
				if (do_buf_alloc(fd_, TESSAI_MEMORY_RPU,
						 kFwRspBoSize, &bo) != 0 ||
				    bo == 0) {
					++fails;
					return;
				}
				if (do_gem_close(fd_, bo) != 0)
					++fails;
			}
		});
	}
	for (auto &th : threads)
		th.join();
	EXPECT_EQ(fails.load(), 0)
		<< "alloc/close stress saw " << fails.load() << " failures";
}
