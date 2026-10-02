// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// amdrnpu gtest harness: shared helpers + the AmdrnpuTest
// fixture that opens /dev/amdrnpu.N once per test, closes in TearDown,
// and skips cleanly (GTEST_SKIP) when the driver isn't loaded so we
// don't hard-fail CI runs on dev hosts.
//
// The suite drives the device through libamdrnpu (amdrnpu_buf_*,
// amdrnpu_cmd_submit, amdrnpu_syncobj_*): that is the supported
// userspace entry point for the char-driver UAPI, so the tests
// exercise the exact code path production consumers use. The do_*
// wrappers below are thin adapters that keep the per-test call sites
// small and give each op a single, greppable failure point. Linking
// libamdrnpu also keeps the suite cross-buildable against any sysroot
// that ships the amdrnpu UAPI header (include/uapi/amdrnpu/amdrnpu.h).

#ifndef AMDRNPU_TEST_COMMON_H_
#define AMDRNPU_TEST_COMMON_H_

#include <gtest/gtest.h>

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <amdrnpu/amdrnpu.h>
#include <amdrnpu/amdrnpu_lib.h>

#include "utils.h"  // utest_common

namespace amdrnpu_test {

// ---------------------------------------------------------------------
// Magic firmware opcodes (kept in lockstep with the RPU slave's
// amdrnpu endpoint handler table).
// ---------------------------------------------------------------------

// fw_echo writeback: firmware snprintfs "rsp: command id: %u,
// num_args: %u\n" into args[0]'s RPU-bank carveout when this opcode
// is dispatched.
constexpr uint32_t kFwEchoOp     = 0x42424242u;
constexpr uint32_t kFwRspTextMax = 400u;
constexpr uint32_t kFwRspBoSize  = 4096u;

// Partial-window round-trip opcode (fw_echo_region). Wire args (uint64_t each):
//   args[0] = dst_pa  (carveout PA of an RPU-bank BO)
//   args[1] = offset  (8-byte aligned bytes into the BO)
//   args[2] = length  (16..kFwEchoRegionMax bytes of the partial window)
// RPU reads 8 bytes at dst_pa+offset (echoes back what TO_DEVICE flushed),
// then writes "region seq=%u off=%llu len=%llu in=0x%016llx\0<0xAA pad>"
// into [dst_pa+offset, dst_pa+offset+length). Bytes OUTSIDE that range
// are never touched -- BufSyncPartialRoundTrip uses 0xCC/0x33 sentinels
// around the window to verify both the RPU honored the length cap AND
// the partial BUF_SYNC didn't widen to whole-BO.
// Magic: 'R','G','N','E' so it's visible on rpmsg traces.
constexpr uint32_t kFwOpEchoRegion  = 0x52474E45u;
constexpr uint32_t kFwEchoRegionMax = 400u;

// VEK385 DEVICE-bank PA window -- the reservation that bank@1 in our
// DT binds to via `memory-region = <0x01>` (numeric phandle, not a
// label; the official Versal-net /reserved-memory layout doesn't
// label these pools, so the amdrnpu node references the pool
// by its declared phandle value).
//
// Phandle 0x01 resolves to the high-DDR pool declared by the
// official base DT as:
//
//   buffer@50000000000 {
//       reg              = <0x500 0x0 0x1 0x0>;   // base 0x500_0000_0000, size 4 GiB
//       compatible       = "shared-dma-pool";
//       reusable;
//       phandle          = <0x01>;
//   };
//
// The system-wide CMA fallback (`linux,cma-default`) lives on the
// SEPARATE 512 MiB RPU pool (`rpu_gem_cma@20000000`, phandle 0x500)
// in the official layout -- NOT on this 4 GiB high-DDR pool. So this
// device bank is, in practice, dedicated to consumers that bind it
// by explicit memory-region phandle (today: amdrnpu's bank@1).
//
// To verify the live layout matches these constants on the board:
//   ssh root@vek385 'hexdump -e "\" 0x%08x\"\n"
//       /proc/device-tree/reserved-memory/buffer@50000000000/reg'
// then byte-swap each printed u32 (the printout is native-LE; dt
// cells are stored BE) and reassemble as
// <addr_hi addr_lo size_hi size_lo> u32 tuples -- should produce
// 0x500 0x0 0x1 0x0.
//
// If the base DTB ever changes the pool size, bump kDeviceShmemSize
// to match.
//
// Used by AmdrnpuCmd.DeviceShmemAddrRoundTrip to assert that the
// PA the kernel patched into the wire (and that the RPU echoed back)
// really landed inside the DEVICE bank.
constexpr uint64_t kDeviceShmemBase = 0x50000000000ull;  // 0x500_0000_0000 = buffer@50000000000 base
constexpr uint64_t kDeviceShmemSize = 0x000100000000ull;  // 4 GiB (matches dts <0x500 0x0 0x1 0x0>)
constexpr uint64_t kDeviceShmemEnd  = kDeviceShmemBase + kDeviceShmemSize;

// Device-shmem address-echo opcode (DAPP). Wire args (uint64_t each):
//   args[0] = ack_pa     (RPU-bank carveout PA, writeback target)
//   args[1] = device_pa  (DEVICE-bank PA being acknowledged; opaque
//                         to the RPU -- NEVER dereferenced because
//                         device shmem at 0x500_0000_0000+ is outside
//                         the R5_1 MPU window)
//   args[2] = device_sz  (scalar bytes of the device-shmem region)
// RPU writes "dev-ping pa=0x%016llx size=%llu seq=%u" into
// [ack_pa, ack_pa + kFwDappBannerMax) and leaves the device-shmem BO
// untouched. Userspace sscanf's the banner to verify the wire
// round-trip carried the kernel-patched PA + size faithfully back.
// Magic: 'D','A','P','P' so it's visible on rpmsg traces.
// Used by AmdrnpuCmd.DeviceShmemAddrRoundTrip.
constexpr uint32_t kFwOpDeviceAddrPing = 0x44415050u;
constexpr uint32_t kFwDappBannerMax    = 128u;

// Fault-injection opcodes -- firmware sets the matching
// command_rsp_type on the wire so the kernel maps to a specific
// dma_fence errno. Documented in amdrnpu docs/commands.md §3.
constexpr uint32_t kFwOpFaultRemoteIo  = 0x52454D49u;  // 'R','E','M','I'
constexpr uint32_t kFwOpFaultNotSupp   = 0x4E4F5453u;  // 'N','O','T','S'
constexpr uint32_t kFwOpFaultCancelled = 0x43414E43u;  // 'C','A','N','C'
constexpr uint32_t kFwOpFaultBadRsp    = 0x42414443u;  // 'B','A','D','C'

// Per-submit wall-clock cap. Generous enough that even a fully-loaded
// RPU still completes within bound but tight enough that a hung
// firmware causes a fast FAIL rather than dragging CI to a halt.
constexpr uint64_t kSubmitTimeoutNs = 2ull * 1'000'000'000ull;

// ---------------------------------------------------------------------
// Device discovery.
// ---------------------------------------------------------------------

// Iterates /dev/amdrnpu.N entries, returns the first fd opened
// O_RDWR|O_CLOEXEC. @out_path is populated on success. Returns -1 on
// failure (caller should GTEST_SKIP).
int open_amdrnpu_device(char *out_path = nullptr, size_t out_sz = 0);

// Cheap fd-liveness probe: 0 if @fd is a valid open descriptor,
// -errno (e.g. -EBADF after close) otherwise. Used to assert RAII
// close semantics without leaning on any device ioctl.
int probe_fd(int fd);

// ---------------------------------------------------------------------
// Time helpers.
// ---------------------------------------------------------------------

uint64_t now_ns();

// ---------------------------------------------------------------------
// Wire-protocol primitives. Each wraps one libamdrnpu call so the
// per-test call sites stay small and every op has a single failure
// point. Return convention: 0 on success, negative errno on failure.
// ---------------------------------------------------------------------

// amdrnpu_cmd_submit(). @args may be null when @num_args == 0. Returns
// 0 on success and writes the completion sync_file fd into @out_handle
// (the rest of the suite treats it as an opaque "syncobj" handle).
int do_cmd_submit(int fd, uint32_t command_op,
		  const struct amdrnpu_arg *args, uint32_t num_args,
		  uint32_t *out_handle);

// amdrnpu_syncobj_wait_complete() with @timeout_ns. Returns 0 once the
// fence is signalled (including signalled-with-error -- callers inspect
// status separately), -ETIMEDOUT on deadline, negative errno on error.
// Does NOT close the handle (caller manages lifecycle).
int do_syncobj_wait(int fd, uint32_t handle, uint64_t timeout_ns);

// Release a completion handle (close the sync_file fd).
int do_syncobj_destroy(int fd, uint32_t handle);

// amdrnpu_buf_alloc(). Writes the driver's per-file import handle.
int do_buf_alloc(int fd, uint32_t mtype, uint64_t size, uint32_t *out_handle);

// amdrnpu_buf_close() -- drops the import handle for @handle.
int do_gem_close(int fd, uint32_t handle);

// amdrnpu_buf_mmap() -- maps the whole BO for @handle R/W SHARED and
// writes the mapping into @out_map. Pair with do_buf_munmap().
int do_buf_mmap(int fd, uint32_t handle, uint64_t size, void **out_map);

// amdrnpu_buf_munmap() -- undo do_buf_mmap().
void do_buf_munmap(void *addr, uint64_t length);

// amdrnpu_buf_sync(). @offset/@length are reserved (the underlying
// DMA_BUF_IOCTL_SYNC is whole-buffer) and kept only so call sites read
// intent-first.
int do_buf_sync(int fd, uint32_t handle, uint32_t direction,
		uint64_t offset, uint64_t length);

// amdrnpu_syncobj_to_fence_fd() -- dups the completion fd. Caller owns
// @out_sf_fd.
int do_syncobj_to_fence_fd(int fd, uint32_t handle, int *out_sf_fd);

// Non-blocking fence status via amdrnpu_syncobj_fence_status():
//   >0 signaled-ok (typically 1)
//   =0 still pending
//   <0 dma_fence error (e.g. -EREMOTEIO)
int do_syncobj_fence_status(int fd, uint32_t handle, int32_t *out_status);

// amdrnpu_syncobj_wait_busy_poll(). NOTE: this API is OBSOLETE and
// delegates to wait_complete (there is no way to query command status
// ahead of the response message); the @interval_ns arg is accepted for
// source compatibility but ignored. Does NOT close the handle.
int do_syncobj_busy_poll(int fd, uint32_t handle,
			 uint64_t max_wall_ns, uint64_t interval_ns,
			 int32_t *out_status);

// ---------------------------------------------------------------------
// EL0 userspace dcache invalidate -- only for the inflight-pin subtest
// where the import handle was already closed before the fence
// signaled, so amdrnpu_buf_sync() is unreachable. No-op on non-arm64.
// ---------------------------------------------------------------------
void uspace_dcache_invalidate(const void *vaddr, size_t length);

// ---------------------------------------------------------------------
// gtest fixture: opens the amdrnpu char device, skips if missing.
//
// Inherit from this fixture in any TEST_F that needs an open device fd.
// Pure-discovery / negative tests that want to probe without a working
// device can use raw TEST() + open_amdrnpu_device() directly.
// ---------------------------------------------------------------------
class AmdrnpuTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		char path[PATH_MAX] = {};
		fd_ = open_amdrnpu_device(path, sizeof(path));
		if (fd_ < 0)
			GTEST_SKIP() << "no /dev/amdrnpu.N found; "
				     << "is amdrnpu.ko loaded and the "
				     << "rpmsg channel bound?";
		card_path_ = path;
	}

	void TearDown() override
	{
		if (fd_ >= 0) {
			::close(fd_);
			fd_ = -1;
		}
	}

	int fd_ = -1;
	std::string card_path_;
};

}  // namespace amdrnpu_test

#endif  // AMDRNPU_TEST_COMMON_H_
