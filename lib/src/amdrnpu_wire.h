/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
/*
 * amdrnpu_wire - host<->RPU message layout for the userspace rpmsg_lite path.
 *
 * Mirrors the kernel driver's wire contract (drivers/rpmsg/amdrnpu/
 * amdrnpu_cmd.h): little-endian command/response structs carried as rpmsg
 * payloads on the "amdrnpu" channel.  Kept in the library so userspace can
 * speak to the same firmware without the kernel module.
 */
#ifndef AMDRNPU_WIRE_H_
#define AMDRNPU_WIRE_H_

#include <stdint.h>

/* rpmsg channel name the RPU firmware announces (AMDRNPU_RPMSG_NAME). */
#define AMDRNPU_WIRE_CHANNEL "amdrnpu"

/*
 * host -> RPU; args are little-endian 64-bit words.  The field layout is
 * naturally padding-free (four u32 then a u64 array at offset 16), so the wire
 * bytes are identical to the kernel driver's __packed struct.
 */
struct amdrnpu_wire_cmd {
	uint32_t seq;
	uint32_t command_op;
	uint32_t flags;
	uint32_t num_args;
	uint64_t args[];
};

/* RPU -> host. */
struct amdrnpu_wire_rsp {
	uint32_t seq;
	uint32_t command_rsp_type;
};

_Static_assert(sizeof(struct amdrnpu_wire_cmd) == 16, "wire cmd header layout");
_Static_assert(sizeof(struct amdrnpu_wire_rsp) == 8, "wire rsp layout");

/* Wire response discriminator; indices match the kernel's rsp->errno table. */
enum amdrnpu_wire_rsp_type {
	AMDRNPU_WIRE_RSP_COMPLETE	= 0,
	AMDRNPU_WIRE_RSP_REMOTEIO	= 1,
	AMDRNPU_WIRE_RSP_NOTSUPPORTED	= 2,
	AMDRNPU_WIRE_RSP_CANCELLED	= 3,
};

#endif /* AMDRNPU_WIRE_H_ */
