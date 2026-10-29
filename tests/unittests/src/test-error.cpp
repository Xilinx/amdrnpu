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
//
// These drive libamdrnpu, so the negatives assert the lib's contract
// (0 / -errno return convention) rather than raw ioctl errnos.

#include "common.h"

#include <array>
#include <atomic>
#include <thread>

using namespace amdrnpu_test;

class AmdrnpuError : public AmdrnpuTest {};

// ---- buffer alloc negative cases -----------------------------------

TEST_F(AmdrnpuError, BufAllocInvalidMemoryType)
{
	// memory_type values outside AMDRNPU_MEMORY_* are rejected before
	// any ioctl is issued.
	uint32_t bo = 0;
	EXPECT_EQ(do_buf_alloc(fd_, 0xDEADBEEFu, 4096, &bo), -EINVAL);
}

TEST_F(AmdrnpuError, BufAllocZeroSize)
{
	// size=0 has no defensible meaning; the lib rejects it up front.
	uint32_t bo = 0;
	EXPECT_EQ(do_buf_alloc(fd_, AMDRNPU_MEMORY_RPU, 0, &bo), -EINVAL);
}

// ---- buffer sync negative cases ------------------------------------

TEST_F(AmdrnpuError, BufSyncInvalidHandle)
{
	// Unknown handle isn't in the lib's registry -> -ENOENT (the
	// assertion is just "fails cleanly, doesn't crash the kernel").
	EXPECT_LT(do_buf_sync(fd_, 0xDEADu, AMDRNPU_BUF_SYNC_TO_DEVICE,
			      0, 4096),
		  0);
}

TEST_F(AmdrnpuError, BufSyncInvalidDirection)
{
	// Allocate a real BO, then pass an invalid direction.
	uint32_t bo = 0;
	ASSERT_EQ(do_buf_alloc(fd_, AMDRNPU_MEMORY_RPU, kFwRspBoSize, &bo), 0);

	// 0xFF is neither TO_DEVICE nor FROM_DEVICE.
	EXPECT_EQ(do_buf_sync(fd_, bo, 0xFFu, 0, 0), -EINVAL);

	EXPECT_EQ(do_gem_close(fd_, bo), 0);
}

// ---- buffer close negative case ------------------------------------

TEST_F(AmdrnpuError, BufCloseInvalidHandle)
{
	// Closing a handle the lib never handed out -> -ENOENT.
	EXPECT_LT(do_gem_close(fd_, 0xDEADu), 0);
}

// ---- cmd submit negative case --------------------------------------

// num_args > 0 with a NULL args pointer is a caller bug the lib rejects
// before touching the driver. This is the libamdrnpu analogue of the
// old raw "userspace lies about num_args" canary.
TEST_F(AmdrnpuError, CmdSubmitNullArgsWithCount)
{
	uint32_t syncobj = 0;
	EXPECT_EQ(do_cmd_submit(fd_, 0xCAFEBABEu, nullptr, 2, &syncobj),
		  -EINVAL);
}

// ---- completion-handle negative cases ------------------------------

TEST_F(AmdrnpuError, SyncobjWaitOnInvalidHandle)
{
	// A completion handle is a sync_file fd; an fd we never opened
	// polls as POLLNVAL, which the lib maps to a negative return.
	int32_t status = 0;
	EXPECT_LT(amdrnpu_syncobj_wait_complete(fd_, 0x7FFFFFFEu,
						10'000'000 /* 10ms */,
						&status),
		  0);
}

TEST_F(AmdrnpuError, SyncobjDestroyTwiceFails)
{
	// First submit -> get a real completion handle -> release it ->
	// release again should fail (the fd is already closed).
	uint32_t syncobj = 0;
	ASSERT_EQ(do_cmd_submit(fd_, 0xBAADF00Du, nullptr, 0, &syncobj), 0);
	ASSERT_EQ(do_syncobj_wait(fd_, syncobj, kSubmitTimeoutNs), 0);
	EXPECT_EQ(do_syncobj_destroy(fd_, syncobj), 0);
	EXPECT_LT(do_syncobj_destroy(fd_, syncobj), 0);
}

// ---- open negative -------------------------------------------------

TEST(AmdrnpuErrorNegative, OpenNonexistentDevicePath)
{
	// /dev/amdrnpu.99 doesn't exist on any amdrnpu host.
	int device_fd = ::open("/dev/amdrnpu.99", O_RDWR | O_CLOEXEC);
	EXPECT_LT(device_fd, 0);
	if (device_fd >= 0)
		::close(device_fd);
}

// ---- multi-threaded variant ----------------------------------------

// Pound buf_alloc + buf_close in a tight loop across threads. Catches
// races in the per-file import allocation / freeing paths (we've seen
// "BO leaked after close" regressions in this kind of stress).
TEST_F(AmdrnpuError, BufAllocCloseStressMultiThreaded)
{
	constexpr int kNumThreads = 4;
	constexpr int kCyclesPerThread = 64;
	std::atomic<int> fails{0};
	std::array<std::thread, kNumThreads> threads;
	for (int t = 0; t < kNumThreads; ++t) {
		threads[t] = std::thread([&] {
			for (int i = 0; i < kCyclesPerThread; ++i) {
				uint32_t bo = 0;
				if (do_buf_alloc(fd_, AMDRNPU_MEMORY_RPU,
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
