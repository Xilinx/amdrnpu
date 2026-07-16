/* SPDX-License-Identifier: GPL-2.0 */
/*
 * amdrnpu — RPU/host RPMsg wire reference.
 *
 * Private to drivers/accel/amdrnpu.  Submission layout:
 * `<uapi/amdrnpu/amdrnpu.h>` (`struct amdrnpu_cmd_submit`).
 *
 * Commands complete when the RPU returns a terminal response; success or failure
 * is reflected only via the syncobj-backed dma_fence (dma_fence_set_error on fault).
 */
#ifndef _AMDRNPU_CMD_H_
#define _AMDRNPU_CMD_H_

#include <linux/types.h>

/* ------------------------------------------------------------------ */
/* RPMsg host → RPU                                                   */
/* ------------------------------------------------------------------ */

/**
 * Layout matches firmware `prc_cmd` on the wire.
 *
 * @seq: command id from amdrnpu xa_alloc_cyclic (xa_limit_32b, reused when free).
 *       In-flight cmds live in cmd_xa at this index.  Userspace and firmware do
 *       not assign ids.
 */
struct __attribute__((__packed__)) amdrnpu_wire_cmd {
	__le32 seq;
	__le32 command_op;
	__le32 flags;
	__le32 num_args;
	__le64 args[];
};

/**
 * enum amdrnpu_rsp_type — RPU → host (wire discriminator).
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
	AMDRNPU_RSP_COMPLETE	= 0,
	AMDRNPU_RSP_REMOTEIO	= 1,
	AMDRNPU_RSP_NOTSUPPORTED	= 2,
	AMDRNPU_RSP_CANCELLED	= 3,
};

#define AMDRNPU_RSP_ERROR AMDRNPU_RSP_REMOTEIO

struct __attribute__((__packed__)) amdrnpu_wire_rsp {
	__le32 seq;		/* echo of driver-assigned command id from submit */
	__le32 command_rsp_type;
};

#endif /* _AMDRNPU_CMD_H_ */
