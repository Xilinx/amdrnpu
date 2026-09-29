// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Device-level probes: node discovery, ABI version surface, multi-fd
// open/close, and concurrent open/close behavior.
//
// Device-level coverage: UAPI ABI version check, multi-fd open/close,
// and per-fd thread-safety of the submit path. New coverage gained
// when this gtest tree replaced the older C smoke binary (which only
// sanity-checked the driver name once in main()).

#include "common.h"

#include <array>
#include <atomic>
#include <thread>

using namespace amdrnpu_test;

class AmdrnpuDevice : public AmdrnpuTest {};

// ---- discovery / version probe ------------------------------------

TEST_F(AmdrnpuDevice, AbiVersionMatchesUapiHeader)
{
	// Until we publish a dedicated GET_INFO ioctl, the UAPI header's
	// AMDRNPU_ABI_VERSION is the contract userspace consults.
	// Fail loudly if it ever moves below 1 (which would mean the
	// header was reverted across the sync_ioctl + ABI v1 work).
	ASSERT_GE(AMDRNPU_ABI_VERSION, 1);
}

// ---- multiple opens of the same device -----------------------------

// Standalone (TEST, not TEST_F) because this manages its own array of
// fds rather than the fixture's single fd. Lives in a dedicated suite
// because gtest forbids mixing TEST_F and TEST within one suite name.
TEST(AmdrnpuDeviceStandalone, MultipleOpenClose)
{
	// Each amdrnpu_open allocates a per-file context (xa, mm).
	// Open the card N times, do a noop submit on each, close in
	// reverse order. Catches "stale per-file state shared across
	// fds" regressions.
	constexpr int kNumFds = 4;
	std::array<int, kNumFds> fds{};
	for (int i = 0; i < kNumFds; ++i) {
		fds[i] = open_amdrnpu_device();
		if (fds[i] < 0) {
			for (int j = 0; j < i; ++j)
				::close(fds[j]);
			GTEST_SKIP() << "no /dev/amdrnpu.N found";
		}
	}
	for (int i = 0; i < kNumFds; ++i) {
		uint32_t syncobj = 0;
		ASSERT_EQ(do_cmd_submit(fds[i], 0xD0000000u + i, nullptr, 0,
					&syncobj),
			  0);
		EXPECT_EQ(do_syncobj_wait(fds[i], syncobj, kSubmitTimeoutNs), 0);
		EXPECT_EQ(do_syncobj_destroy(fds[i], syncobj), 0);
	}
	for (int i = kNumFds - 1; i >= 0; --i)
		EXPECT_EQ(::close(fds[i]), 0);
}

// ---- concurrent submit on a single fd ------------------------------

// Two threads, same fd, race to submit. Validates the per-file ioctl
// path is safe under contention (amdrnpu uses RCU/idr internally
// for cmd_xa; if that breaks, this test deadlocks/asserts).
TEST_F(AmdrnpuDevice, ConcurrentSubmitSingleFd)
{
	constexpr int kNumThreads = 4;
	constexpr int kSubmitsPerThread = 8;
	std::atomic<int> fails{0};
	std::array<std::thread, kNumThreads> threads;
	for (int t = 0; t < kNumThreads; ++t) {
		threads[t] = std::thread([&, t] {
			for (int i = 0; i < kSubmitsPerThread; ++i) {
				uint32_t syncobj = 0;
				int rc = do_cmd_submit(
					fd_,
					0xE0000000u | (t << 16) | i,
					nullptr, 0, &syncobj);
				if (rc != 0 || syncobj == 0) {
					++fails;
					return;
				}
				if (do_syncobj_wait(fd_, syncobj,
						    kSubmitTimeoutNs) != 0)
					++fails;
				(void)do_syncobj_destroy(fd_, syncobj);
			}
		});
	}
	for (auto &th : threads)
		th.join();
	EXPECT_EQ(fails.load(), 0)
		<< "concurrent submits failed " << fails.load() << " times";
}
