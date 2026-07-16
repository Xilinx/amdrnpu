/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * amdrnpu <-> RPURuntime wire-protocol type definitions: the bare
 * request/response structs and the rsp_type discriminator. Opcodes,
 * size caps and RPU-bank carveout bounds live in
 * amdrnpu_endpoint.h (endpoint contract, not pure wire shape).
 *
 * Byte-compatible with the rpu-fw mirror
 * (rpu-fw/rpu_fw/app/include/transport/amdrnpu_protocol.h) and
 * the userspace surface (tessai-agent/tests/unittests/src/common.h,
 * lib/examples/amdrnpu_test.c).
 */

#ifndef AMDRNPU_MSG_DEF_H_
#define AMDRNPU_MSG_DEF_H_

#include <stdint.h>

#include <zephyr/sys/util.h>     /* BUILD_ASSERT */
#include <zephyr/toolchain.h>    /* __packed */

#ifdef __cplusplus
extern "C" {
#endif

/* Wire rsp_type discriminator (matches APU-side fence error mapping). */
enum amdrnpu_rsp_type {
	AMDRNPU_RSP_COMPLETE     = 0,
	AMDRNPU_RSP_REMOTEIO     = 1,
	AMDRNPU_RSP_NOTSUPPORTED = 2,
	AMDRNPU_RSP_CANCELLED    = 3,
};

/* Inbound command from APU. args[] is num_args uint64_t entries. */
struct amdrnpu_wire_cmd {
	uint32_t seq;
	uint32_t command_op;
	uint32_t flags;
	uint32_t num_args;
	uint64_t args[];
} __packed;

/* Outbound short response header (8 wire bytes + 16 reserved). */
struct amdrnpu_rsp_wire_hdr {
	uint32_t seq;
	uint32_t command_rsp_type;
	uint32_t reserved[4];
} __packed;

BUILD_ASSERT(sizeof(struct amdrnpu_rsp_wire_hdr) == 24,
	     "amdrnpu response header layout drift");

#ifdef __cplusplus
}
#endif

#endif /* AMDRNPU_MSG_DEF_H_ */
