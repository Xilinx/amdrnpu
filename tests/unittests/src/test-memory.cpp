// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Buffer-object lifecycle: alloc, mmap, buf_sync (cache coherency),
// page-aligned partial mmap, free.
//
// The happy path (BO alloc -> mmap -> TO_DEVICE sync -> submit ->
// FROM_DEVICE sync -> readback) is covered end-to-end by
// TessaiAgentCmd.FwEchoWriteback; here we split each step into its
// own TEST_F so a regression in (e.g.) MAP_DUMB shows up without
// dragging the entire firmware round-trip in. The partial-window
// round trip (TO_DEVICE+FROM_DEVICE on a sub-page range, validated
// via the RPU's fw_echo_region opcode) lives at the bottom in
// TessaiAgentMemory.BufSyncPartialRoundTrip.

#include "common.h"

#include <array>

using namespace tessai_test;

class TessaiAgentMemory : public TessaiAgentTest {};

// ---- alloc / close happy path -------------------------------------

TEST_F(TessaiAgentMemory, BufAllocRpuBank)
{
	uint32_t bo = 0;
	ASSERT_EQ(do_buf_alloc(fd_, TESSAI_MEMORY_RPU, kFwRspBoSize, &bo), 0);
	EXPECT_NE(bo, 0u);
	EXPECT_EQ(do_gem_close(fd_, bo), 0);
}

TEST_F(TessaiAgentMemory, BufAllocDeviceBank)
{
	uint32_t bo = 0;
	ASSERT_EQ(do_buf_alloc(fd_, TESSAI_MEMORY_DEVICE, MB(1), &bo), 0);
	EXPECT_NE(bo, 0u);
	EXPECT_EQ(do_gem_close(fd_, bo), 0);
}

// ---- mmap + memset round-trip -------------------------------------

TEST_F(TessaiAgentMemory, BufMmapAndMemsetRpuBank)
{
	uint32_t bo = 0;
	ASSERT_EQ(do_buf_alloc(fd_, TESSAI_MEMORY_RPU, kFwRspBoSize, &bo), 0);

	uint64_t off = 0;
	ASSERT_EQ(do_map_dumb(fd_, bo, &off), 0);

	void *p = ::mmap(nullptr, kFwRspBoSize, PROT_READ | PROT_WRITE,
			 MAP_SHARED, fd_, static_cast<off_t>(off));
	ASSERT_NE(p, MAP_FAILED);

	// Touch every page so the page-fault path actually fires.
	std::memset(p, 0xA5, kFwRspBoSize);
	// First + last byte sanity check (catches "mmap returned a stale
	// kernel page that bypasses the GEM range").
	EXPECT_EQ(static_cast<uint8_t *>(p)[0], 0xA5);
	EXPECT_EQ(static_cast<uint8_t *>(p)[kFwRspBoSize - 1], 0xA5);

	EXPECT_EQ(::munmap(p, kFwRspBoSize), 0);
	EXPECT_EQ(do_gem_close(fd_, bo), 0);
}

// ---- TESSAI_BUF_SYNC ----------------------------------------------

TEST_F(TessaiAgentMemory, BufSyncBothDirectionsWholeBuffer)
{
	// length=0 means "whole BO from @offset". Both directions must
	// succeed against a freshly-allocated bank BO -- we're not
	// validating any cache effect here, just the ioctl shape.
	uint32_t bo = 0;
	ASSERT_EQ(do_buf_alloc(fd_, TESSAI_MEMORY_RPU, kFwRspBoSize, &bo), 0);

	EXPECT_EQ(do_buf_sync(fd_, bo, TESSAI_BUF_SYNC_TO_DEVICE, 0, 0), 0);
	EXPECT_EQ(do_buf_sync(fd_, bo, TESSAI_BUF_SYNC_FROM_DEVICE, 0, 0), 0);

	EXPECT_EQ(do_gem_close(fd_, bo), 0);
}

TEST_F(TessaiAgentMemory, BufSyncPartialRange)
{
	// Sync only the second page of a multi-page BO.
	uint32_t bo = 0;
	uint64_t bo_size = 2 * static_cast<uint64_t>(get_page_size());
	ASSERT_EQ(do_buf_alloc(fd_, TESSAI_MEMORY_RPU, bo_size, &bo), 0);

	uint64_t page = static_cast<uint64_t>(get_page_size());
	EXPECT_EQ(do_buf_sync(fd_, bo, TESSAI_BUF_SYNC_TO_DEVICE,
			      page, page),
		  0);

	EXPECT_EQ(do_gem_close(fd_, bo), 0);
}

// ---- TESSAI_BUF_SYNC partial-window FULL ROUND-TRIP ----------------
//
// The BufSyncPartialRange test above only validates the ioctl SHAPE
// (driver accepts (offset, length) within the BO). It does NOT prove
// that cache maintenance reaches device memory for the requested
// window only -- a buggy driver that always flushes whole-BO, or one
// that silently truncates to page granularity, would still pass.
//
// This test closes that gap with an end-to-end exchange through the
// RPU's TESSAI_FW_OP_ECHO_REGION ('RGNE') opcode, which on the
// firmware side (see RPURuntime tessai_agent_endpoint.c:
// tessai_fw_echo_region) does two things in one round trip:
//
//   1. Reads 8 bytes at the window's start. Whatever userspace
//      flushed via BUF_SYNC TO_DEVICE before submit ends up in the
//      banner the RPU writes back. If TO_DEVICE was a no-op (or
//      silently widened/narrowed), we either see zeros / the
//      previous tenant or fail the bounds sentinel.
//
//   2. Writes a "region seq=%u off=%llu len=%llu in=0x%016llx"
//      banner over the partial window, then pads the rest of the
//      window with 0xAA. Userspace pulls those bytes back via
//      BUF_SYNC FROM_DEVICE and verifies (a) the banner sscanf's
//      and the echoed input matches what we wrote pre-flush
//      (proves TO_DEVICE worked), AND (b) the sentinel bytes
//      AROUND the window (0xCC before, 0x33 after) are unchanged
//      (proves the RPU honored the length cap AND the partial
//      sync didn't accidentally invalidate cache lines outside
//      the requested range).
//
// Layout (BO = 2 pages; window is the middle 256 bytes of page 1):
//
//   page 0 ............................ pre-fill 0xCC, never synced
//   page 1 [0 .. winoff)            pre-fill 0x33, never synced
//          [winoff .. winoff+winlen) -- partial window  (RPU writes)
//          [winoff+winlen .. page)   pre-fill 0x33, never synced
//
// Skip cleanly if 'RGNE' isn't recognized (older firmware build):
// the wait will still succeed (the firmware sends COMPLETE for
// every unknown opcode), but the banner sscanf will fail. We
// distinguish "old firmware" from "real regression" by checking
// whether the window bytes still equal the pre-submit 0x33 fill
// -- if they do, the firmware never wrote anything (= unknown
// opcode); if they're partially modified, it's a real bug.
TEST_F(TessaiAgentMemory, BufSyncPartialRoundTrip)
{
	const uint64_t page    = static_cast<uint64_t>(get_page_size());
	const uint64_t bo_size = 2 * page;
	const uint64_t winoff  = page + 128ull;        // 8-byte aligned
	const uint64_t winlen  = 256ull;               // 16 <= len <= 400
	ASSERT_LE(winoff + winlen, bo_size);

	uint32_t bo = 0;
	ASSERT_EQ(do_buf_alloc(fd_, TESSAI_MEMORY_RPU, bo_size, &bo), 0);

	uint64_t mmap_off = 0;
	ASSERT_EQ(do_map_dumb(fd_, bo, &mmap_off), 0);

	void *map = ::mmap(nullptr, bo_size, PROT_READ | PROT_WRITE,
			   MAP_SHARED, fd_, static_cast<off_t>(mmap_off));
	ASSERT_NE(map, MAP_FAILED);
	uint8_t *base = static_cast<uint8_t *>(map);

	// Sentinel fill: 0xCC for the entire first page (never synced),
	// 0x33 for the second page (only [winoff, winoff+winlen) will
	// be synced). The pre/post window bytes inside page 1 give us
	// a second sentinel class so a "RPU clobbered too much" bug
	// shows up as 0x33 -> 0xAA (or banner text) rather than the
	// CC/anything ambiguity the first-page sentinel can't catch.
	std::memset(base,        0xCC, page);
	std::memset(base + page, 0x33, page);

	// Stamp the magic 8-byte input echo at the START of the window.
	// The RPU reads this and embeds it in the banner; we'll compare
	// the echoed value back to here to prove TO_DEVICE actually
	// reached memory. 64 bits of entropy so a collision with stale
	// carveout content is impossible in any real run.
	constexpr uint64_t kInputEcho = 0xDEADBEEFCAFEBABEull;
	std::memcpy(base + winoff, &kInputEcho, sizeof(kInputEcho));

	// Partial flush of JUST the window. If the driver silently
	// promoted this to whole-BO, the pre/post-window 0x33 sentinels
	// would still survive (writeback's a strict no-op outside the
	// window) so we can't detect promotion in the TO_DEVICE
	// direction directly -- the kInputEcho echo via the banner is
	// what proves the flush happened at all.
	ASSERT_EQ(do_buf_sync(fd_, bo, TESSAI_BUF_SYNC_TO_DEVICE,
			      winoff, winlen),
		  0);

	std::array<struct tessai_arg, 3> args{};
	args[0].size  = static_cast<uint32_t>(winlen);
	args[0].bo.drm_buffer_handle = static_cast<int32_t>(bo);
	args[0].bo.offset = 0;
	args[1].size  = 0;
	args[1].value = winoff;
	args[2].size  = 0;
	args[2].value = winlen;

	uint32_t syncobj = 0;
	ASSERT_EQ(do_cmd_submit(fd_, kFwOpEchoRegion, args.data(),
				static_cast<uint32_t>(args.size()), &syncobj),
		  0);

	ASSERT_EQ(do_syncobj_wait(fd_, syncobj, kSubmitTimeoutNs), 0);
	(void)do_syncobj_destroy(fd_, syncobj);

	// Partial invalidate of JUST the window. If the driver
	// silently widened this to whole-BO, the page-0 0xCC sentinel
	// would still be valid (RPU never wrote there, so no stale
	// dcache to invalidate) -- so we can only validate widening
	// via the pre/post-window 0x33 bytes inside page 1, which DO
	// share cache lines with the window. A widening-bug variant
	// that ALSO drops the 0x33 lines from the cache would leave
	// 0x33 in memory (RPU never touched it), so userspace would
	// still observe 0x33 after the invalidate. That means we can't
	// detect FROM_DEVICE widening via this geometry alone -- the
	// strict assertion here is "FROM_DEVICE on the window made the
	// RPU's writes visible", and the widening-into-CC sentinel
	// further down catches the more dangerous "RPU wrote outside
	// the window" failure mode.
	ASSERT_EQ(do_buf_sync(fd_, bo, TESSAI_BUF_SYNC_FROM_DEVICE,
			      winoff, winlen),
		  0);

	// Parse the banner the RPU wrote. The format string MUST stay
	// in lockstep with the snprintf in
	// RPURuntime/.../tessai_agent_endpoint.c:tessai_fw_echo_region
	// AND with the mirror in
	// tessai-agent/tests/rpu_app/.../tessai_agent_endpoint.c.
	unsigned int  rpu_seq = 0;
	unsigned long long rpu_off = 0, rpu_len = 0, rpu_in = 0;
	int matched = std::sscanf(reinterpret_cast<const char *>(base + winoff),
				  "region seq=%u off=%llu len=%llu in=0x%llx",
				  &rpu_seq, &rpu_off, &rpu_len, &rpu_in);

	// "Old firmware build" branch: every byte of the window still
	// matches the pre-submit 0x33 fill -> RPU treated 'RGNE' as
	// unknown opcode (sent COMPLETE without writing anything).
	// SKIP rather than FAIL so flashing an older slave doesn't
	// turn the whole suite red.
	bool window_untouched = true;
	for (uint64_t i = 0; i < winlen; ++i) {
		if (base[winoff + i] != 0x33) {
			window_untouched = false;
			break;
		}
	}
	if (matched != 4 && window_untouched) {
		EXPECT_EQ(::munmap(map, bo_size), 0);
		EXPECT_EQ(do_gem_close(fd_, bo), 0);
		GTEST_SKIP() << "RPU firmware does not recognize 'RGNE' "
				"(TESSAI_FW_OP_ECHO_REGION) -- rebuild "
				"rpu_app_link_a.elf from "
				"tests/rpu_app/ to pick up the partial-"
				"sync round-trip handler";
		return;
	}

	if (matched != 4) {
		std::cerr << "  partial roundtrip banner parse failed; "
			     "first 64 bytes of window:";
		for (int i = 0; i < 64 && i < (int)winlen; ++i) {
			if (i % 16 == 0) std::cerr << "\n  ";
			std::fprintf(stderr, "%02x ",
				     base[winoff + i]);
		}
		std::cerr << std::endl;
	}
	ASSERT_EQ(matched, 4)
		<< "RPU banner missing or malformed at base+0x"
		<< std::hex << winoff;

	// TO_DEVICE proof: the echoed 8-byte input matches what we
	// wrote pre-flush. If TO_DEVICE was a no-op, in= would be
	// 0x3333333333333333 (the page-1 sentinel still in memory)
	// or the previous carveout tenant.
	EXPECT_EQ(rpu_in, kInputEcho)
		<< "RPU echoed 0x" << std::hex << rpu_in
		<< " but userspace flushed 0x" << kInputEcho
		<< " -- BUF_SYNC TO_DEVICE did not reach memory for "
		<< "[off=" << std::dec << winoff << ", len=" << winlen << ")";

	// Banner self-consistency.
	EXPECT_EQ(rpu_off, winoff);
	EXPECT_EQ(rpu_len, winlen);

	// PRE-window byte (last byte of page 1 before window) must be
	// untouched 0x33. RPU widening into [winoff-1, winoff) would
	// leave a banner-prefix character or 0xAA pad here.
	EXPECT_EQ(base[winoff - 1], 0x33u)
		<< "byte at off=" << (winoff - 1)
		<< " was clobbered (RPU wrote outside the partial window)";

	// POST-window byte must be untouched 0x33. RPU widening into
	// [winoff+winlen, winoff+winlen+1) would leave 0xAA pad or
	// banner-tail content here.
	EXPECT_EQ(base[winoff + winlen], 0x33u)
		<< "byte at off=" << (winoff + winlen)
		<< " was clobbered (RPU wrote past the partial window)";

	// PAGE-0 byte must be untouched 0xCC. RPU widening into a
	// different page (which can only happen via a bounds-check
	// regression in the firmware handler) would leave whatever
	// the firmware wrote here. Catches the worst-case "ignored
	// offset entirely" failure mode.
	EXPECT_EQ(base[0], 0xCCu)
		<< "page 0 sentinel clobbered (RPU ignored args[1] offset "
		<< "and wrote to dst_pa+0?)";

	// LAST byte of window must be 0xAA pad (from the RPU's pad
	// loop). Catches "RPU wrote a short banner and left the rest
	// untouched" -- a regression where the snprintf was shortened
	// without updating the pad memset would leave a 0x33 tail.
	EXPECT_EQ(base[winoff + winlen - 1], 0xAAu)
		<< "last byte of partial window is 0x"
		<< std::hex << static_cast<unsigned>(base[winoff + winlen - 1])
		<< " (expected 0xAA pad from fw_echo_region)";

	EXPECT_EQ(::munmap(map, bo_size), 0);
	EXPECT_EQ(do_gem_close(fd_, bo), 0);
}

// ---- deterministic fill/verify pair -------------------------------

// Allocate -> mmap -> fill_buffer32 with the testcases-v2-style
// pattern -> verify_buffer32. Doesn't involve the RPU; just exercises
// the driver's mmap path and confirms userspace can both read and
// write a BO with the expected byte pattern.
TEST_F(TessaiAgentMemory, BufMmapFillVerifyPattern)
{
	uint32_t bo = 0;
	const size_t size = KB(64);
	ASSERT_EQ(do_buf_alloc(fd_, TESSAI_MEMORY_RPU, size, &bo), 0);

	uint64_t off = 0;
	ASSERT_EQ(do_map_dumb(fd_, bo, &off), 0);

	void *p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
			 MAP_SHARED, fd_, static_cast<off_t>(off));
	ASSERT_NE(p, MAP_FAILED);

	fill_buffer32(p, size);
	EXPECT_TRUE(verify_buffer32(p, size));

	EXPECT_EQ(::munmap(p, size), 0);
	EXPECT_EQ(do_gem_close(fd_, bo), 0);
}

// ---- large allocation cap (sanity) --------------------------------

TEST_F(TessaiAgentMemory, BufAllocLargeRequestEitherSucceedsOrEinval)
{
	// 1 GiB request from the DEVICE bank. On VEK385 bank@1 binds to
	// buffer@50000000000 (4 GiB reusable shared-dma-pool), so this
	// should EITHER fail cleanly (CMA fragmented / driver cap hit)
	// OR succeed (1 GiB fits with room to spare in 4 GiB). The test
	// asserts only "no kernel crash / no hang"; either return shape
	// is acceptable.
	uint32_t bo = 0;
	int rc = do_buf_alloc(fd_, TESSAI_MEMORY_DEVICE, GB(1), &bo);
	if (rc == 0) {
		EXPECT_NE(bo, 0u);
		EXPECT_EQ(do_gem_close(fd_, bo), 0);
	} else {
		EXPECT_LT(rc, 0);
	}
}
