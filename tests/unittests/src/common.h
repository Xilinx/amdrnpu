// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// tessai-agent gtest harness: shared helpers + the TessaiAgentTest
// fixture that opens /dev/dri/cardN once per test, closes in TearDown,
// and skips cleanly (GTEST_SKIP) when the driver isn't loaded so we
// don't hard-fail CI runs on dev hosts.
//
// Every helper here is libdrm-free: tessai_agent_lib already wraps
// these ioctls for production users, but the kernel UAPI surface
// (drm.h, drm_mode.h, sync_file.h, tessai-agent/tessai_agent.h) is
// what we actually want to test. Calling raw ioctls also keeps the
// unittest binary cross-buildable against any sysroot that ships
// kernel UAPI headers -- the same constraint the retired
// tests/src/tessai_lifecycle.c libdrm-free C smoke binary lived
// under before this gtest tree took over its role.

#ifndef TESSAI_AGENT_TEST_COMMON_H_
#define TESSAI_AGENT_TEST_COMMON_H_

#include <gtest/gtest.h>

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <linux/sync_file.h>

#include <tessai-agent/tessai_agent.h>

#include "utils.h"  // utest_common

namespace tessai_test {

// ---------------------------------------------------------------------
// Magic firmware opcodes (kept in lockstep with the slave's
// tessai_agent_endpoint.c -- mirror copy at
// tests/rpu_app/src/driver/src/tessai_agent_endpoint.c).
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
// label these pools, so the tessai-agent node references the pool
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
// by explicit memory-region phandle (today: tessai-agent's bank@1).
//
// To verify the live layout matches these constants on the board:
//   ssh root@vek385 'hexdump -e "\" 0x%08x\"\n" \
//       /proc/device-tree/reserved-memory/buffer@50000000000/reg'
// then byte-swap each printed u32 (the printout is native-LE; dt
// cells are stored BE) and reassemble as
// <addr_hi addr_lo size_hi size_lo> u32 tuples -- should produce
// 0x500 0x0 0x1 0x0.
//
// If the base DTB ever changes the pool size, bump kDeviceShmemSize
// to match.
//
// Used by TessaiAgentCmd.DeviceShmemAddrRoundTrip to assert that the
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
// Used by TessaiAgentCmd.DeviceShmemAddrRoundTrip.
constexpr uint32_t kFwOpDeviceAddrPing = 0x44415050u;
constexpr uint32_t kFwDappBannerMax    = 128u;

// Fault-injection opcodes -- firmware sets the matching
// command_rsp_type on the wire so the kernel maps to a specific
// dma_fence errno. Documented in tessai-agent docs/commands.md §3.
constexpr uint32_t kFwOpFaultRemoteIo  = 0x52454D49u;  // 'R','E','M','I'
constexpr uint32_t kFwOpFaultNotSupp   = 0x4E4F5453u;  // 'N','O','T','S'
constexpr uint32_t kFwOpFaultCancelled = 0x43414E43u;  // 'C','A','N','C'
constexpr uint32_t kFwOpFaultBadRsp    = 0x42414443u;  // 'B','A','D','C'

// Older kernel UAPI headers may not expose the EXPORT_SYNC_FILE flag
// even though every kernel that ships the tessai-agent driver does
// support it. Local fallback keeps the test compilable against the
// Vitis sysroot's headers.
#ifndef DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE
#define DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE (1u << 0)
#endif

// Per-submit wall-clock cap. Generous enough that even a fully-loaded
// RPU still completes within bound but tight enough that a hung
// firmware causes a fast FAIL rather than dragging CI to a halt.
constexpr uint64_t kSubmitTimeoutNs = 2ull * 1'000'000'000ull;

// ---------------------------------------------------------------------
// DRM device discovery.
// ---------------------------------------------------------------------

// Reads DRM_IOCTL_VERSION.name into @out; returns 0 or -errno.
int drm_get_name(int fd, char *out, size_t out_sz);

// Iterates /dev/dri/cardN entries, returns the first fd whose driver
// name is TESSAI_AGENT_DRIVER_NAME. @out_path is populated on success.
// Returns -1 on failure (caller should GTEST_SKIP).
int open_tessai_card(char *out_path = nullptr, size_t out_sz = 0);

// ---------------------------------------------------------------------
// Time helpers.
// ---------------------------------------------------------------------

uint64_t now_ns();

// ---------------------------------------------------------------------
// Wire-protocol primitives. Each wraps one ioctl directly; the
// raw-ioctl style (rather than going through libtessai_agent) is
// what lets the suite probe negative paths and bound-checks that
// libtessai_agent guards against on the userspace side.
// ---------------------------------------------------------------------

// TESSAI_CMD_SUBMIT. @args may be null when @num_args == 0. Returns 0
// on success and writes the kernel-allocated syncobj handle.
int do_cmd_submit(int fd, uint32_t command_op,
		  const struct tessai_arg *args, uint32_t num_args,
		  uint32_t *out_handle);

// DRM_IOCTL_SYNCOBJ_WAIT with @timeout_ns from now. Does NOT destroy
// the syncobj (caller manages lifecycle).
int do_syncobj_wait(int fd, uint32_t handle, uint64_t timeout_ns);

// DRM_IOCTL_SYNCOBJ_DESTROY.
int do_syncobj_destroy(int fd, uint32_t handle);

// TESSAI_BUF_ALLOC.
int do_buf_alloc(int fd, uint32_t mtype, uint64_t size, uint32_t *out_handle);

// DRM_IOCTL_GEM_CLOSE.
int do_gem_close(int fd, uint32_t handle);

// DRM_IOCTL_MODE_MAP_DUMB -- gets the mmap offset for @handle.
int do_map_dumb(int fd, uint32_t handle, uint64_t *out_offset);

// TESSAI_BUF_SYNC. length=0 means "@offset..end".
int do_buf_sync(int fd, uint32_t handle, uint32_t direction,
		uint64_t offset, uint64_t length);

// DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD + EXPORT_SYNC_FILE. Caller owns @out_sf_fd.
int do_syncobj_to_fence_fd(int fd, uint32_t handle, int *out_sf_fd);

// Non-blocking fence_status: exports a sync_file fd, asks
// SYNC_IOC_FILE_INFO, closes. status semantics:
//   >0 signaled-ok (typically 1)
//   =0 still pending
//   <0 dma_fence error (e.g. -EREMOTEIO)
int do_syncobj_fence_status(int fd, uint32_t handle, int32_t *out_status);

// Loop fence_status until terminal or @max_wall_ns elapses. Sleeps
// @interval_ns between probes; 0 means sched_yield(). Does NOT destroy
// (subtests inspect status after this returns).
int do_syncobj_busy_poll(int fd, uint32_t handle,
			 uint64_t max_wall_ns, uint64_t interval_ns,
			 int32_t *out_status);

// ---------------------------------------------------------------------
// EL0 userspace dcache invalidate -- only for the inflight-pin subtest
// where the GEM handle was already GEM_CLOSE'd before the syncobj
// signaled, so TESSAI_BUF_SYNC is unreachable. No-op on non-arm64.
// ---------------------------------------------------------------------
void uspace_dcache_invalidate(const void *vaddr, size_t length);

// ---------------------------------------------------------------------
// gtest fixture: opens the tessai-agent DRM card, skips if missing.
//
// Inherit from this fixture in any TEST_F that needs an open card fd.
// Pure-discovery / negative tests that want to probe without a working
// device can use raw TEST() + open_tessai_card() directly.
// ---------------------------------------------------------------------
class TessaiAgentTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		char path[PATH_MAX] = {};
		fd_ = open_tessai_card(path, sizeof(path));
		if (fd_ < 0)
			GTEST_SKIP() << "tessai-agent DRM card not found; "
				     << "is tessai_agent.ko loaded and the "
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

}  // namespace tessai_test

#endif  // TESSAI_AGENT_TEST_COMMON_H_
