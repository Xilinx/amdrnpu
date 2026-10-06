/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note OR MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
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

/**
 * struct amdrnpu_arg — one submission argument (scalar or buffer slice).
 *
 * Scalar: @size == 0, @value is a QWORD on the wire.
 * Buffer: @size != 0, @bo references a buffer previously imported with
 *	   AMDRNPU_BUF_IMPORT; the driver emits (dma_addr + @bo.offset) as the
 *	   wire word.  @bo.buffer_handle is the handle returned by the import,
 *	   and [@bo.offset, @bo.offset + @size) must lie within the buffer.
 */
struct amdrnpu_arg {
	__u64 size;
	union {
		__u64 value;
		struct {
			__s32 buffer_handle;
			__u32 offset;
		} bo;
	};
};

/**
 * struct amdrnpu_cmd_submit - AMDRNPU_CMD_SUBMIT header plus args[] (flex array).
 *
 * sync_handle: zero on entry; on success the kernel returns a
 * completion handle for this submission that userspace waits on.  The driver
 * is an rpmsg char device and the handle is a sync_file fd (poll() for
 * POLLIN).  command_op and num_args select the firmware opcode and the
 * trailing amdrnpu_arg array.  Driver-owned wire seq is not UAPI.
 *
 * Scalar args (@size == 0, @value) and buffer args (@size != 0, @bo, from
 * AMDRNPU_BUF_IMPORT) are both accepted.
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

/*
 * struct amdrnpu_buf_import.flags - target DMA domain for the imported buffer.
 *
 * Selects which device the dma-buf is attached to, i.e. whose DMA address is
 * programmed on the wire when the buffer is used as a command argument:
 *
 *   AMDRNPU_BUF_IMPORT_DEVICE	the accelerator (misc) device.  This is the
 *				default and is what a plain buffer argument uses.
 *   AMDRNPU_BUF_IMPORT_FW	the RPU/firmware device: the buffer is mapped in
 *				the RPU's DMA domain and attached to the rpmsg
 *				remoteproc device, so the emitted address is the
 *				one the RPU dereferences.
 *
 * The two targets are mutually exclusive; all other bits are reserved and must
 * be 0.
 */
#define AMDRNPU_BUF_IMPORT_DEVICE	0u
#define AMDRNPU_BUF_IMPORT_FW		(1u << 0)

/**
 * struct amdrnpu_buf_import - import an external dma-buf for use as a
 * command buffer argument.
 *
 * @fd:		dma-buf file descriptor to import.  Typically allocated by
 *		userspace from a dma-buf heap (e.g. /dev/dma_heap/reserved);
 *		the buffer must be physically contiguous and, to be reachable
 *		by the RPU, allocated from the RPU-visible pool.
 * @flags:	target DMA domain, one of AMDRNPU_BUF_IMPORT_DEVICE (0) or
 *		AMDRNPU_BUF_IMPORT_FW; other bits reserved, must be 0.
 * @handle:	out; opaque buffer handle for struct amdrnpu_arg.bo.buffer_handle.
 * @pad:	reserved, must be 0.
 *
 * The handle is valid until it is released with AMDRNPU_BUF_DESTROY or the
 * opening file description is closed.  Cache maintenance and CPU mapping are
 * performed on the dma-buf fd directly (mmap(2), DMA_BUF_IOCTL_SYNC), not
 * through this driver.
 */
struct amdrnpu_buf_import {
	__s32 fd;
	__u32 flags;
	__u32 handle;
	__u32 pad;
};

#define AMDRNPU_BUF_IMPORT \
	_IOWR(AMDRNPU_IOC_MAGIC, 0x10, struct amdrnpu_buf_import)

/**
 * struct amdrnpu_buf_destroy - release a buffer handle from AMDRNPU_BUF_IMPORT.
 *
 * @handle:	buffer handle to release.
 * @pad:	reserved, must be 0.
 *
 * Drops the driver's reference to the imported dma-buf for @handle and removes
 * it from the file's handle table.  If the buffer is still referenced by an
 * in-flight command, the underlying dma-buf attachment is not torn down until
 * that command completes.  After this call @handle is invalid and must not be
 * used in AMDRNPU_CMD_SUBMIT.
 */
struct amdrnpu_buf_destroy {
	__u32 handle;
	__u32 pad;
};

#define AMDRNPU_BUF_DESTROY \
	_IOW(AMDRNPU_IOC_MAGIC, 0x11, struct amdrnpu_buf_destroy)

/**
 * struct amdrnpu_buf_query - return the DMA address recorded at import.
 *
 * @handle:	buffer handle from AMDRNPU_BUF_IMPORT / amdrnpu_buf_alloc().
 * @pad:	reserved, must be 0.
 * @dma_addr:	out; device/firmware-domain physical address of the buffer.
 * @size:	out; contiguous mapped length in bytes.
 *
 * Used by userspace when filling APU_XRT_TEST descriptor karg entries
 * (absolute PAs inside the descriptor blob).  The same address is what
 * AMDRNPU_CMD_SUBMIT emits for wire BO args.
 */
struct amdrnpu_buf_query {
	__u32 handle;
	__u32 pad;
	__u64 dma_addr;
	__u64 size;
};

#define AMDRNPU_BUF_QUERY \
	_IOWR(AMDRNPU_IOC_MAGIC, 0x12, struct amdrnpu_buf_query)

/**
 * enum amdrnpu_memory_type - target memory domain for amdrnpu_buf_alloc().
 *
 * AMDRNPU_MEMORY_RPU:	  RPU/firmware-reachable memory.  The library allocates
 *			  from the RPU dma-buf heap and imports the buffer in
 *			  the firmware (remoteproc) DMA domain.
 * AMDRNPU_MEMORY_DEVICE:  accelerator/device memory (default).  Allocated from
 *			  the device dma-buf heap and imported in the device
 *			  DMA domain.
 */
enum amdrnpu_memory_type {
	AMDRNPU_MEMORY_RPU	= 0,
	AMDRNPU_MEMORY_DEVICE	= 1,
};

#define AMDRNPU_MEMORY_DEFAULT	AMDRNPU_MEMORY_DEVICE

/**
 * enum amdrnpu_buf_sync_dir - direction for amdrnpu_buf_sync().
 *
 * AMDRNPU_BUF_SYNC_TO_DEVICE:	 CPU wrote the buffer; flush so the device sees it.
 * AMDRNPU_BUF_SYNC_FROM_DEVICE: device wrote the buffer; invalidate before CPU read.
 */
enum amdrnpu_buf_sync_dir {
	AMDRNPU_BUF_SYNC_TO_DEVICE	= 0,
	AMDRNPU_BUF_SYNC_FROM_DEVICE	= 1,
};

#endif /* _UAPI_AMDRNPU_H_ */
