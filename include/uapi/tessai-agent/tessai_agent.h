/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note OR MIT */
/*
 * tessai-agent — userspace/kernel ABI (ioctl surface)
 *
 * Custom ioctls (magic `'T'`): buffer allocation (`TESSAI_BUF_ALLOC`) and command
 * submission (`TESSAI_CMD_SUBMIT`).  RPMsg/driver packing is not part of UAPI —
 * see the driver-internal header bundled with kernel sources / lib trees.
 *
 * Standard DRM also exposes drm_syncobj and GEM on the same render node file.
 */
#ifndef _UAPI_TESSAI_AGENT_H_
#define _UAPI_TESSAI_AGENT_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define TESSAI_AGENT_DRIVER_NAME	"tessai_agent"
#define TESSAI_AGENT_ABI_VERSION	13

#ifndef TESSAI_IOC_MAGIC
#define TESSAI_IOC_MAGIC			'T'
#endif

/* ------------------------------------------------------------------ */
/* Buffer allocation (custom ioctl)                                   */
/* ------------------------------------------------------------------ */

enum tessai_memory_type {
	TESSAI_MEMORY_RPU		= 0,
	TESSAI_MEMORY_DEVICE	= 1,
};
#define TESSAI_MEMORY_DEFAULT	TESSAI_MEMORY_DEVICE

struct tessai_buf_alloc {
	__u32 flags;
	__u32 memory_type;
	__u64 size;
	__u32 drm_handle;
	__u32 reserved0;
};

/* ------------------------------------------------------------------ */
/* Command submission (TESSAI_CMD_SUBMIT)                                 */
/* ------------------------------------------------------------------ */

/**
 * struct tessai_arg — one submission argument (scalar or buffer GEM slice).
 *
 * Scalar: @size == 0, @value is a QWORD on the wire.
 * Buffer: @size != 0, @bo resolves to dma_addr / RPU window on the RPMsg side.
 */
struct tessai_arg {
	__u64 size;
	union {
		__u64 value;
		struct {
			__s32 drm_buffer_handle;
			__u32 offset;
		} bo;
	};
};

/**
 * struct tessai_cmd_submit — user-visible TESSAI_CMD_SUBMIT header plus @args[].
 *
 * @drm_sync_obj_handle: submit with 0; on success the kernel returns the
 * drm_syncobj handle for this command lifetime.
 * @command_op / @num_args: firmware operation selector and trailing @tessai_arg count.
 *
 * RPMsg correlator packing (EXEC path) is synthesized inside the driver and is not
 * part of UAPI.
 */
struct tessai_cmd_submit {
	__s32 drm_sync_obj_handle;
	__u32 command_op;
	__u32 num_args;
	struct tessai_arg args[];
};

#define TESSAI_CMD_SUBMIT			_IOWR(TESSAI_IOC_MAGIC, 0x20, struct tessai_cmd_submit)

/* ------------------------------------------------------------------ */
/* IOCTL numbers (magic 'T')                                           */
/* ------------------------------------------------------------------ */

#define TESSAI_BUF_ALLOC			_IOWR(TESSAI_IOC_MAGIC, 0x10, struct tessai_buf_alloc)

#endif /* _UAPI_TESSAI_AGENT_H_ */
