/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tessai-agent — RPU/host RPMsg wire reference.
 *
 * Private to drivers/accel/tessai-agent.  Submission layout:
 * `<uapi/tessai-agent/tessai_agent.h>` (`struct tessai_cmd_submit`).
 *
 * Commands complete when the RPU returns a terminal response; success or failure
 * is reflected only via the syncobj-backed dma_fence (dma_fence_set_error on fault).
 */
#ifndef _TESSAI_AGENT_CMD_H_
#define _TESSAI_AGENT_CMD_H_

#include <linux/types.h>

/* ------------------------------------------------------------------ */
/* RPMsg host → RPU                                                   */
/* ------------------------------------------------------------------ */

/**
 * Layout matches firmware `prc_cmd` on the wire.
 *
 * @seq: command id from tessai_agent xa_alloc_cyclic (xa_limit_32b, reused when free).
 *       In-flight cmds live in cmd_xa at this index.  Userspace and firmware do
 *       not assign ids.
 */
struct __attribute__((__packed__)) tessai_wire_cmd {
	__le32 seq;
	__le32 command_op;
	__le32 flags;
	__le32 num_args;
	__le64 args[];
};

/**
 * enum tessai_rsp_type — RPU → host (wire discriminator).
 *
 * Terminal codes set dma_fence status:
 *	TESSAI_RSP_COMPLETE	(success)
 *	TESSAI_RSP_REMOTEIO	-EREMOTEIO
 *	TESSAI_RSP_NOTSUPPORTED	-EOPNOTSUPP
 *	TESSAI_RSP_CANCELLED	-ECANCELED
 *
 * Any other numeric wire value is treated as a fault: the driver logs and
 * completes the command with dma_fence error -EREMOTEIO.
 */
enum tessai_rsp_type {
	TESSAI_RSP_COMPLETE	= 0,
	TESSAI_RSP_REMOTEIO	= 1,
	TESSAI_RSP_NOTSUPPORTED	= 2,
	TESSAI_RSP_CANCELLED	= 3,
};

#define TESSAI_RSP_ERROR TESSAI_RSP_REMOTEIO

struct __attribute__((__packed__)) tessai_wire_rsp {
	__le32 seq;		/* echo of driver-assigned command id from submit */
	__le32 command_rsp_type;
};

#endif /* _TESSAI_AGENT_CMD_H_ */
