// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Channel-up + scalar-args smoke tests. Anything that doesn't touch
// a BO, a sync_file, or a fault opcode lives here.
//
// Tests in this file:
//   - AmdrnpuBasic.DriverNameIsAmdrnpu  (drm_get_name probe)
//   - AmdrnpuBasic.NoopSubmit               (num_args=0 round trip)
//   - AmdrnpuBasic.ScalarArgsSubmit         (scalar __u64 args)
//   - AmdrnpuBasic.FwEchoOpSubmitScalarOnly (0x42424242 dispatch)
// Plus the hip-style multi-threaded variant:
//   - AmdrnpuBasicStandalone.NoopSubmitMultiThreaded

#include "common.h"

#include <array>
#include <atomic>
#include <thread>

using namespace amdrnpu_test;

// Per-file fixture alias keeps the gtest suite name unique
// (--gtest_filter=AmdrnpuBasic.*) while inheriting SetUp/TearDown
// from AmdrnpuTest. Same idiom in every test-*.cpp here.
class AmdrnpuBasic : public AmdrnpuTest {};

TEST_F(AmdrnpuBasic, DriverNameIsAmdrnpu)
{
	// Sanity check the fixture: open_amdrnpu_card has already verified
	// drm_get_name == AMDRNPU_DRIVER_NAME, but re-probe here so a
	// regression in the discovery loop surfaces as a per-test failure
	// instead of a fleet-wide GTEST_SKIP.
	char name[64] = {};
	ASSERT_EQ(drm_get_name(fd_, name, sizeof(name)), 0);
	ASSERT_STREQ(name, AMDRNPU_DRIVER_NAME);
}

// ---- smallest possible round-trip ---------------------------------

TEST_F(AmdrnpuBasic, NoopSubmit)
{
	// 16-byte amdrnpu_wire_cmd request, 24-byte amdrnpu_rsp_wire_hdr
	// response. Validates channel-up + COMPLETE shape.
	uint32_t syncobj = 0;
	ASSERT_EQ(do_cmd_submit(fd_, 0xCAFEBABEu, nullptr, 0, &syncobj), 0);
	ASSERT_NE(syncobj, 0u);
	EXPECT_EQ(do_syncobj_wait(fd_, syncobj, kSubmitTimeoutNs), 0);
	EXPECT_EQ(do_syncobj_destroy(fd_, syncobj), 0);
}

// ---- scalar args pass-through -------------------------------------

TEST_F(AmdrnpuBasic, ScalarArgsSubmit)
{
	// Validates amdrnpu_resolve_args's "size==0 -> cpu_to_le64(value)"
	// path: scalar args traverse the driver without GEM lookups.
	std::array<struct amdrnpu_arg, 2> args{};
	args[0].size  = 0;
	args[0].value = 0xC0FFEE0011223344ull;
	args[1].size  = 0;
	args[1].value = 0xBADCAFE055667788ull;

	uint32_t syncobj = 0;
	ASSERT_EQ(do_cmd_submit(fd_, 0x12345678u, args.data(),
				static_cast<uint32_t>(args.size()), &syncobj),
		  0);
	ASSERT_NE(syncobj, 0u);
	EXPECT_EQ(do_syncobj_wait(fd_, syncobj, kSubmitTimeoutNs), 0);
	EXPECT_EQ(do_syncobj_destroy(fd_, syncobj), 0);
}

// ---- fw_echo magic op without writeback ---------------------------

TEST_F(AmdrnpuBasic, FwEchoOpSubmitScalarOnly)
{
	// Firmware doesn't writeback when args[0] is scalar (no carveout
	// dma_addr); only the COMPLETE path is exercised here. The
	// full writeback round trip lives in test-cmd.cpp
	// (AmdrnpuCmd.FwEchoWriteback).
	std::array<struct amdrnpu_arg, 2> args{};
	args[0].size  = 0;
	args[0].value = 0xDEADBEEFCAFE0001ull;
	args[1].size  = 0;
	args[1].value = 0xDEADBEEFCAFE0002ull;

	uint32_t syncobj = 0;
	ASSERT_EQ(do_cmd_submit(fd_, kFwEchoOp, args.data(),
				static_cast<uint32_t>(args.size()), &syncobj),
		  0);
	ASSERT_NE(syncobj, 0u);
	EXPECT_EQ(do_syncobj_wait(fd_, syncobj, kSubmitTimeoutNs), 0);
	EXPECT_EQ(do_syncobj_destroy(fd_, syncobj), 0);
}

// ---- multi-threaded smoke -----------------------------------------

// Mirrors HipXrtDevice.HipXrtDeviceInfoBasic's "spin N threads, each
// runs the basic flow, join all" idiom. Each thread opens its OWN fd
// so we exercise multi-client open/close in addition to multi-thread
// submit. Lives in its own suite (AmdrnpuBasicStandalone) because
// gtest forbids mixing TEST_F and TEST within the same suite name.
//
// SKIP must be raised from the main test thread; child threads that
// call GTEST_SKIP just exit the lambda silently and the parent test
// reports PASS even though nothing ran. Probe the device once up
// front, skip in main if absent, then launch workers.
TEST(AmdrnpuBasicStandalone, NoopSubmitMultiThreaded)
{
	int probe_fd = open_amdrnpu_card();
	if (probe_fd < 0)
		GTEST_SKIP() << "amdrnpu DRM card not found";
	::close(probe_fd);

	constexpr int kNumThreads = 4;
	std::atomic<int> fails{0};
	std::array<std::thread, kNumThreads> threads;
	for (int i = 0; i < kNumThreads; ++i) {
		threads[i] = std::thread([&] {
			int fd = open_amdrnpu_card();
			if (fd < 0) { ++fails; return; }
			uint32_t syncobj = 0;
			if (do_cmd_submit(fd, 0xC0FFEE00u, nullptr, 0,
					  &syncobj) != 0 || syncobj == 0) {
				++fails;
				::close(fd);
				return;
			}
			if (do_syncobj_wait(fd, syncobj, kSubmitTimeoutNs) != 0)
				++fails;
			(void)do_syncobj_destroy(fd, syncobj);
			::close(fd);
		});
	}
	for (auto &t : threads)
		t.join();
	EXPECT_EQ(fails.load(), 0)
		<< fails.load() << " of " << kNumThreads
		<< " threads failed";
}
