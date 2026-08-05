/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note OR MIT */
/*
 * amdrnpu — userspace/kernel ABI (ioctl surface)
 *
 * Custom ioctl (magic `'R'` for rnpu): command submission (`AMDRNPU_CMD_SUBMIT`).
 * RPMsg/driver packing is not part of UAPI — see the driver-internal header
 * bundled with kernel sources / lib trees.
 */
#ifndef _UAPI_AMDRNPU_H_
#define _UAPI_AMDRNPU_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#define AMDRNPU_DRIVER_NAME	"amdrnpu"
#define AMDRNPU_ABI_VERSION	1

#ifndef AMDRNPU_IOC_MAGIC
#define AMDRNPU_IOC_MAGIC			'R'
#endif

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
 * sync_handle: zero on entry; on success the kernel returns a
 * completion handle for this submission that userspace waits on. the driver is 
 * an rpmsg char device and the handle is a sync_file fd
 * (poll() for POLLIN); once the driver becomes a DRM device it is a
 * drm_syncobj handle.  command_op and num_args select the firmware opcode and
 * the trailing amdrnpu_arg array.  Driver-owned wire seq is not UAPI.
 *
 * Only scalar args (@size == 0, @value) are accepted for now
 *
 * Example (OP_CODE_ECHO, two scalar args):
 *
 *	#define OP_CODE_ECHO	0x00000001u
 *
 *	size_t cmd_len = sizeof(struct amdrnpu_cmd_submit)
 *			 + 2 * sizeof(struct amdrnpu_arg);
 *	struct amdrnpu_cmd_submit *cmd = calloc(1, cmd_len);
 *
 *	cmd->sync_handle = 0;
 *	cmd->command_op = OP_CODE_ECHO;
 *	cmd->num_args = 2;
 *	cmd->args[0] = (struct amdrnpu_arg){ .size = 0, .value = 0x1122334455667788ull };
 *	cmd->args[1] = (struct amdrnpu_arg){ .size = 0, .value = 42 };
 *	ioctl(fd, AMDRNPU_CMD_SUBMIT, cmd);
 *	// then poll(&(struct pollfd){ .fd = cmd->sync_handle,
 *	//			      .events = POLLIN }, 1, -1);
 */
struct amdrnpu_cmd_submit {
	__s32 sync_handle;
	__u32 command_op;
	__u32 num_args;
	struct amdrnpu_arg args[];
};

#define AMDRNPU_CMD_SUBMIT \
	_IOWR(AMDRNPU_IOC_MAGIC, 0x20, struct amdrnpu_cmd_submit)

#endif /* _UAPI_AMDRNPU_H_ */
