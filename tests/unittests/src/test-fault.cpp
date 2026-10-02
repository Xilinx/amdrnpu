// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Fault-class fence-errno mapping (Tier B). Requires an RPU slave
// endpoint built with the AMDRNPU_FW_OP_FAULT_* opcodes wired into
// its amdrnpu endpoint handler table.
//
// Tests in this file (mapping kept for tests/README.md cross-ref):
//   - AmdrnpuFault.RemoteIo                ('REMI' -> -EREMOTEIO)
//   - AmdrnpuFault.NotSupported            ('NOTS' -> -EOPNOTSUPP)
//   - AmdrnpuFault.Cancelled               ('CANC' -> -ECANCELED)
//   - AmdrnpuFault.InvalidRspTypeFallback  ('BADC' -> driver-side -EREMOTEIO)
//
// Documented in amdrnpu docs/commands.md §3.

#include "common.h"

using namespace amdrnpu_test;

namespace {

// Each fault opcode causes the endpoint to set a specific
// command_rsp_type on the wire; the driver translates into a
// dma_fence error. SYNC_IOC_FILE_INFO surfaces the error as a
// negative int32_t status. The blocking SYNCOBJ_WAIT here returns 0
// (signaled) even when the fence carries an error -- "signaled with
// error" is still signaled.
void RunFaultClass(int fd, const char *name, uint32_t op,
		   int32_t expected_status)
{
	SCOPED_TRACE(::testing::Message()
		     << name << " (op=0x" << std::hex << op << ")");

	uint32_t syncobj = 0;
	ASSERT_EQ(do_cmd_submit(fd, op, nullptr, 0, &syncobj), 0);
	ASSERT_NE(syncobj, 0u);

	ASSERT_EQ(do_syncobj_wait(fd, syncobj, kSubmitTimeoutNs), 0)
		<< "wait timed out -- endpoint may be running a pre-Phase-2d "
		   "build without the fault opcodes";

	int32_t status = 0;
	ASSERT_EQ(do_syncobj_fence_status(fd, syncobj, &status), 0);
	EXPECT_EQ(status, expected_status)
		<< "fence_status=" << status
		<< " (" << (status < 0 ? std::strerror(-status) : "non-error")
		<< "), expected=" << expected_status
		<< " (" << std::strerror(-expected_status) << ")";

	(void)do_syncobj_destroy(fd, syncobj);
}

}  // anonymous namespace

class AmdrnpuFault : public AmdrnpuTest {};

TEST_F(AmdrnpuFault, RemoteIo)
{
	RunFaultClass(fd_, "REMI -> RSP_REMOTEIO",
		      kFwOpFaultRemoteIo, -EREMOTEIO);
}

TEST_F(AmdrnpuFault, NotSupported)
{
	RunFaultClass(fd_, "NOTS -> RSP_NOTSUPPORTED",
		      kFwOpFaultNotSupp, -EOPNOTSUPP);
}

TEST_F(AmdrnpuFault, Cancelled)
{
	RunFaultClass(fd_, "CANC -> RSP_CANCELLED",
		      kFwOpFaultCancelled, -ECANCELED);
}

// Distinct from the RemoteIo subtest: verifies the DRIVER FALLBACK
// path. Firmware deliberately emits command_rsp_type=99 (out-of-
// range); the driver synthesizes -EREMOTEIO without trusting the
// wire value. commands.md §3: "invalid rsp_type ... completes the
// command with dma_fence error -EREMOTEIO".
TEST_F(AmdrnpuFault, InvalidRspTypeFallback)
{
	RunFaultClass(fd_, "BADC -> rsp_type=99 (driver fallback)",
		      kFwOpFaultBadRsp, -EREMOTEIO);
}
