/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note OR MIT */
/*
 * amdrnpu — userspace/kernel ABI (ioctl surface)
 *
 * Custom ioctls (magic `'T'`): buffer allocation (`AMDRNPU_BUF_ALLOC`) and command
 * submission (`AMDRNPU_CMD_SUBMIT`).  RPMsg/driver packing is not part of UAPI —
 * see the driver-internal header bundled with kernel sources / lib trees.
 *
 * Standard DRM also exposes drm_syncobj and GEM on the same render node file.
 */
#ifndef _UAPI_AMDRNPU_H_
#define _UAPI_AMDRNPU_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define AMDRNPU_DRIVER_NAME	"amdrnpu"
#define AMDRNPU_ABI_VERSION	1

#ifndef AMDRNPU_IOC_MAGIC
#define AMDRNPU_IOC_MAGIC			'T'
#endif

/* ------------------------------------------------------------------ */
/* Buffer allocation (custom ioctl)                                   */
/* ------------------------------------------------------------------ */

enum amdrnpu_memory_type {
	AMDRNPU_MEMORY_RPU		= 0,
	AMDRNPU_MEMORY_DEVICE	= 1,
};

#define AMDRNPU_MEMORY_DEFAULT	AMDRNPU_MEMORY_DEVICE

struct amdrnpu_buf_alloc {
	__u32 flags;
	__u32 memory_type;
	__u64 size;
	__u32 drm_handle;
	__u32 reserved0;
};

/* ------------------------------------------------------------------ */
/* Buffer CPU cache sync (cacheable GEM backing store)                */
/* ------------------------------------------------------------------ */

/**
 * enum amdrnpu_buf_sync_dir - cache maintenance before/after device DMA.
 *
 * Buffers from AMDRNPU_BUF_ALLOC use cacheable CPU mappings; the CPU must
 * flush or invalidate explicitly around device access.
 *
 * AMDRNPU_BUF_SYNC_TO_DEVICE: CPU wrote the range; flush so the device sees it.
 * AMDRNPU_BUF_SYNC_FROM_DEVICE: device wrote the range; invalidate before CPU read.
 */
enum amdrnpu_buf_sync_dir {
	AMDRNPU_BUF_SYNC_TO_DEVICE	= 0,
	AMDRNPU_BUF_SYNC_FROM_DEVICE	= 1,
};

/**
 * struct amdrnpu_buf_sync - partial or full buffer cache sync.
 *
 * @drm_handle: GEM handle from AMDRNPU_BUF_ALLOC.
 * @direction: AMDRNPU_BUF_SYNC_TO_DEVICE or AMDRNPU_BUF_SYNC_FROM_DEVICE.
 * @offset: byte offset into the buffer (0 = start).
 * @length: bytes to sync; 0 means from @offset through end-of-buffer.
 */
struct amdrnpu_buf_sync {
	__u32 drm_handle;
	__u32 direction;
	__u32 flags;
	__u64 offset;
	__u64 length;
};

/* ------------------------------------------------------------------ */
/* Command submission (AMDRNPU_CMD_SUBMIT)                                 */
/* ------------------------------------------------------------------ */

/**
 * struct amdrnpu_arg — one submission argument (scalar or buffer GEM slice).
 *
 * Scalar: @size == 0, @value is a QWORD on the wire.
 * Buffer: @size != 0, @bo resolves to dma_addr / RPU window on the RPMsg side.
 */
struct amdrnpu_arg {
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
 * struct amdrnpu_cmd_submit - AMDRNPU_CMD_SUBMIT header plus args[] (flex array).
 *
 * drm_sync_obj_handle: zero on entry; on success the kernel returns the
 * drm_syncobj handle for this submission.  command_op and num_args select the
 * firmware opcode and the trailing amdrnpu_arg array.  Driver-owned wire seq
 * is not UAPI; buffer args are resolved to DMA addresses before RPMsg.
 *
 * Example (OP_CODE_CMD0, 16 MiB input bo0_hd, 8 MiB output bo1_hd):
 *
 *	#define OP_CODE_CMD0	0x00000001u
 *	#define INPUT_BO	1ull
 *	#define OUTPUT_BO	2ull
 *	#define SZ_16M		(16u * 1024u * 1024u)
 *	#define SZ_8M		(8u * 1024u * 1024u)
 *
 *	size_t cmd_len = sizeof(struct amdrnpu_cmd_submit)
 *			 + 6 * sizeof(struct amdrnpu_arg);
 *	struct amdrnpu_cmd_submit *cmd = calloc(1, cmd_len);
 *
 *	cmd->drm_sync_obj_handle = 0;
 *	cmd->command_op = OP_CODE_CMD0;
 *	cmd->num_args = 6;
 *	cmd->args[0] = (struct amdrnpu_arg){ .size = 0, .value = INPUT_BO };
 *	cmd->args[1] = (struct amdrnpu_arg){ .size = 0, .value = SZ_16M };
 *	cmd->args[2] = (struct amdrnpu_arg){
 *		.size = SZ_16M,
 *		.bo = { .drm_buffer_handle = (__s32)bo0_hd, .offset = 0 },
 *	};
 *	cmd->args[3] = (struct amdrnpu_arg){ .size = 0, .value = OUTPUT_BO };
 *	cmd->args[4] = (struct amdrnpu_arg){ .size = 0, .value = SZ_8M };
 *	cmd->args[5] = (struct amdrnpu_arg){
 *		.size = SZ_8M,
 *		.bo = { .drm_buffer_handle = (__s32)bo1_hd, .offset = 0 },
 *	};
 *	ioctl(drm_fd, AMDRNPU_CMD_SUBMIT, cmd);
 *
 * Via libamdrnpu: amdrnpu_cmd_submit(drm_fd, OP_CODE_CMD0,
 * args, 6, &hdr, sizeof(hdr)) with the same six amdrnpu_arg entries, then
 * amdrnpu_syncobj_wait_complete() on hdr.drm_sync_obj_handle.
 */
struct amdrnpu_cmd_submit {
	__s32 drm_sync_obj_handle;
	__u32 command_op;
	__u32 num_args;
	struct amdrnpu_arg args[];
};

#define AMDRNPU_CMD_SUBMIT \
	_IOWR(AMDRNPU_IOC_MAGIC, 0x20, struct amdrnpu_cmd_submit)

/* ------------------------------------------------------------------ */
/* IOCTL numbers (magic 'T')                                           */
/* ------------------------------------------------------------------ */

#define AMDRNPU_BUF_ALLOC \
	_IOWR(AMDRNPU_IOC_MAGIC, 0x10, struct amdrnpu_buf_alloc)
#define AMDRNPU_BUF_SYNC \
	_IOW(AMDRNPU_IOC_MAGIC, 0x11, struct amdrnpu_buf_sync)

#endif /* _UAPI_AMDRNPU_H_ */
