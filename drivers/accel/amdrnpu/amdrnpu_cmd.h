/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

#ifndef _AMDRNPU_CMD_H_
#define _AMDRNPU_CMD_H_

#include <linux/array_size.h>
#include <linux/errno.h>
#include <linux/types.h>

/**
 * struct amdrnpu_wire_cmd - host -> RPU command; layout matches firmware
 * `prc_cmd` on the wire.
 *
 * @command_op: firmware opcode selecting the RPU handler.
 * @flags: per-command flags (e.g. abort); 0 for a normal submit.
 * @num_args: number of trailing @args entries.
 * @args: flexible array of little-endian argument words.
 *
 * A per-command sequence id is added here later, alongside the driver-side
 * command lifecycle that lets multiple commands be in flight and correlated.
 */
struct __packed amdrnpu_wire_cmd {
	__le32 command_op;
	__le32 flags;
	__le32 num_args;
	__le64 args[];
};

/**
 * enum amdrnpu_rsp_type - RPU -> host (wire discriminator).
 *
 * Terminal codes set dma_fence status:
 *	AMDRNPU_RSP_COMPLETE	(success)
 *	AMDRNPU_RSP_REMOTEIO	-EREMOTEIO
 *	AMDRNPU_RSP_NOTSUPPORTED	-EOPNOTSUPP
 *	AMDRNPU_RSP_CANCELLED	-ECANCELED
 *
 * Any other numeric wire value is treated as a fault: the driver logs and
 * completes the command with dma_fence error -EREMOTEIO.
 */
enum amdrnpu_rsp_type {
	AMDRNPU_RSP_COMPLETE		= 0,
	AMDRNPU_RSP_REMOTEIO		= 1,
	AMDRNPU_RSP_NOTSUPPORTED	= 2,
	AMDRNPU_RSP_CANCELLED		= 3,
};

/* Map a wire response discriminator to a dma_fence errno (0 = success). */
static const int amdrnpu_rsp_to_err[] = {
	[AMDRNPU_RSP_COMPLETE]		= 0,
	[AMDRNPU_RSP_REMOTEIO]		= -EREMOTEIO,
	[AMDRNPU_RSP_NOTSUPPORTED]	= -EOPNOTSUPP,
	[AMDRNPU_RSP_CANCELLED]		= -ECANCELED,
};

static inline int amdrnpu_rsp_err(u32 rsp_type)
{
	if (rsp_type >= ARRAY_SIZE(amdrnpu_rsp_to_err))
		return -EREMOTEIO;
	return amdrnpu_rsp_to_err[rsp_type];
}

struct __packed amdrnpu_wire_rsp {
	__le32 command_rsp_type;
};

#endif /* _AMDRNPU_CMD_H_ */
