/* SPDX-License-Identifier: MIT */
/*
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * Portions of this file consist of AI-generated content.
 */
/*
 * amdrnpu_lib - userspace helpers for the amdrnpu driver.
 *
 * Stable ABI and ioctl layout: `<amdrnpu/amdrnpu.h>`.
 *
 * The public function signatures below are a stable interface; existing
 * callers depend on them, so they must not change (parameter names and
 * documentation may be refined, but arity and types are frozen).  The driver
 * is now an rpmsg char device (`/dev/amdrnpu.N`) rather than a DRM device, so
 * some parameters that only made sense for the old DRM/GEM model are retained
 * for source compatibility and documented as reserved; the library maps each
 * call onto the char-driver ABI internally.
 *
 * Buffer lifecycle: **`amdrnpu_buf_alloc()`** allocates a driver-owned buffer
 * and returns an opaque handle; free it with **`amdrnpu_buf_close()`**.
 * CPU-map a handle with **`amdrnpu_buf_mmap()`** and bracket device DMA with
 * **`amdrnpu_buf_sync()`** (or the direction helpers).
 *
 * Command completion: **`amdrnpu_cmd_submit()`** returns a completion handle in
 * `hdr_out->sync_handle` (a sync_file fd).  Wait/probe it with the
 * **`amdrnpu_syncobj_*`** helpers, or export it for `poll(2)` with
 * **`amdrnpu_syncobj_to_fence_fd()`**.
 */
#ifndef AMDRNPU_LIB_H_
#define AMDRNPU_LIB_H_

#include <stddef.h>
#include <stdint.h>

#include <amdrnpu/amdrnpu.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#  define AMDRNPU_LIB_API
#elif defined(__GNUC__) && __GNUC__ >= 4
#  define AMDRNPU_LIB_API __attribute__((visibility("default")))
#else
#  define AMDRNPU_LIB_API
#endif

/**
 * amdrnpu_buf_alloc - allocate a driver-owned buffer; returns an opaque handle.
 *
 * @device_fd:		open /dev/amdrnpu.N descriptor.
 * @memory_type: target memory domain, an enum amdrnpu_memory_type
 *		(AMDRNPU_MEMORY_DEVICE / AMDRNPU_MEMORY_DEFAULT for device
 *		memory, AMDRNPU_MEMORY_RPU for RPU/firmware memory).  The driver
 *		allocates from the reserved-memory region configured for that
 *		type (AMDRNPU_BUF_ALLOC); requires a driver with that ioctl.
 * @size:	allocation size in bytes.
 * @out_handle:	on success, the opaque handle for use in
 *		struct amdrnpu_arg.bo.buffer_handle and the other buf_* helpers.
 *
 * The library owns the underlying dma-buf fd for the handle; release it with
 * amdrnpu_buf_close().  Returns 0 on success or a negative errno.
 */
AMDRNPU_LIB_API int amdrnpu_buf_alloc(int device_fd, uint32_t memory_type,
				      uint64_t size, uint32_t *out_handle);

/**
 * amdrnpu_buf_close - release a handle from amdrnpu_buf_alloc().
 *
 * @device_fd:		open /dev/amdrnpu.N descriptor (same as passed to buf_alloc).
 * @handle:	handle returned by amdrnpu_buf_alloc().
 *
 * Drops the driver's import and closes the library-owned dma-buf fd.  If a
 * submitted command still references the buffer, the backing memory stays alive
 * until that command completes.  Returns 0 on success or a negative errno.
 */
AMDRNPU_LIB_API int amdrnpu_buf_close(int device_fd, uint32_t handle);

/**
 * amdrnpu_buf_get_phys - return the DMA/physical address of an imported BO.
 *
 * @device_fd:		open /dev/amdrnpu.N descriptor.
 * @handle:	handle from amdrnpu_buf_alloc().
 * @out_pa:	on success, firmware-domain physical address.
 * @out_size:	optional; contiguous mapped size (pass NULL to ignore).
 *
 * Needed when writing absolute PAs into an APU_XRT_TEST descriptor buffer
 * (the driver only rewrites top-level wire BO args, not in-descriptor
 * karg entries).  Returns 0 on success or a negative errno.
 */
AMDRNPU_LIB_API int amdrnpu_buf_get_phys(int device_fd, uint32_t handle,
					 uint64_t *out_pa, uint64_t *out_size);

/**
 * amdrnpu_buf_mmap - CPU-map a (sub)range of a buffer.
 *
 * @device_fd:		open /dev/amdrnpu.N descriptor (same as passed to buf_alloc).
 * @handle:	handle from amdrnpu_buf_alloc().
 * @buf_size:	reserved; the library tracks the allocation size internally and
 *		ignores this argument (pass 0 or the allocation size).
 * @offset:	byte offset into the buffer; must be page-aligned.
 * @length:	bytes to map; 0 maps from @offset through end-of-buffer.
 * @out_map:	on success, the mapping base; unmap with amdrnpu_buf_munmap().
 *
 * The mapping is PROT_READ|PROT_WRITE, MAP_SHARED on the buffer's dma-buf.
 * Returns 0 on success or a negative errno.
 */
AMDRNPU_LIB_API int amdrnpu_buf_mmap(int device_fd, uint32_t handle,
				     uint64_t buf_size,
				     uint64_t offset, uint64_t length,
				     void **out_map);

/** munmap() wrapper; @length must match the mapped length from buf_mmap. */
AMDRNPU_LIB_API void amdrnpu_buf_munmap(void *addr, uint64_t length);

/**
 * amdrnpu_buf_write - copy bytes from userspace into a buffer.
 *
 * @device_fd:		open /dev/amdrnpu.N descriptor (same as passed to buf_alloc).
 * @handle:	handle from amdrnpu_buf_alloc().
 * @src:	source userspace virtual address.
 * @offset:	byte offset into the buffer at which to start writing.
 * @length:	number of bytes to copy.  [@offset, @offset + @length) must lie
 *		within the buffer.
 *
 * Copies bytes from userspace into a buffer with a temporary CPU mapping and
 * memcpy().  Returns 0 on success or a negative errno.
 */
AMDRNPU_LIB_API int amdrnpu_buf_write(int device_fd, uint32_t handle,
				      const void *src, uint64_t offset,
				      uint64_t length);

/**
 * amdrnpu_buf_read - copy bytes from a buffer into userspace.
 *
 * @device_fd:		open /dev/amdrnpu.N descriptor (same as passed to buf_alloc).
 * @handle:	handle from amdrnpu_buf_alloc().
 * @dst:	destination userspace virtual address.
 * @offset:	byte offset into the buffer at which to start reading.
 * @length:	number of bytes to copy.  [@offset, @offset + @length) must lie
 *		within the buffer.
 *
 * Copies bytes from a buffer into userspace with a temporary CPU mapping and
 * memcpy().  Returns 0 on success or a negative errno.
 */
AMDRNPU_LIB_API int amdrnpu_buf_read(int device_fd, uint32_t handle, void *dst,
				     uint64_t offset, uint64_t length);

/**
 * amdrnpu_buf_sync - CPU cache maintenance around device DMA.
 *
 * @device_fd:		open /dev/amdrnpu.N descriptor (same as passed to buf_alloc).
 * @handle:	handle from amdrnpu_buf_alloc().
 * @direction:	AMDRNPU_BUF_SYNC_TO_DEVICE (flush CPU writes) or
 *		AMDRNPU_BUF_SYNC_FROM_DEVICE (invalidate before CPU read).
 * @offset:	reserved; DMA_BUF_IOCTL_SYNC operates on the whole buffer, so
 *		this argument is ignored (pass 0).
 * @length:	reserved; ignored for the same reason (pass 0).
 *
 * Issues DMA_BUF_IOCTL_SYNC on the buffer's dma-buf fd.  Returns 0 on success
 * or a negative errno.
 */
AMDRNPU_LIB_API int amdrnpu_buf_sync(int device_fd, uint32_t handle,
				     uint32_t direction,
				     uint64_t offset, uint64_t length);

/** Flush a CPU-written buffer so device DMA sees it. */
static inline int amdrnpu_buf_sync_to_device(int device_fd, uint32_t handle,
					     uint64_t offset, uint64_t length)
{
	return amdrnpu_buf_sync(device_fd, handle,
				AMDRNPU_BUF_SYNC_TO_DEVICE, offset, length);
}

/** Invalidate a buffer after device DMA before the CPU reads it. */
static inline int amdrnpu_buf_sync_from_device(int device_fd, uint32_t handle,
					       uint64_t offset, uint64_t length)
{
	return amdrnpu_buf_sync(device_fd, handle,
				AMDRNPU_BUF_SYNC_FROM_DEVICE, offset, length);
}

/**
 * amdrnpu_cmd_submit - AMDRNPU_CMD_SUBMIT; sync_file fd in @hdr_out->sync_handle.
 *
 * Builds a variable-length ioctl buffer from @command_op and
 * @args[0 .. @num_args-1] (scalar args with @size == 0, buffer args with
 * @size != 0 referencing a handle from amdrnpu_buf_alloc()).
 *
 * @device_fd:		open /dev/amdrnpu.N descriptor.
 * @hdr_out:	receives the returned header (at least the fixed part); on
 *		success @hdr_out->sync_handle is a sync_file fd the caller owns.
 * @hdr_out_sz:	size of @hdr_out; must be >= offsetof(..., args).
 *
 * Wait for completion with the amdrnpu_syncobj_* helpers (passing
 * @hdr_out->sync_handle), then close(2) the fd.  Returns 0 on success or a
 * negative errno.
 */
AMDRNPU_LIB_API int amdrnpu_cmd_submit(int device_fd,
				       uint32_t command_op,
				       const struct amdrnpu_arg *args,
				       uint32_t num_args,
				       struct amdrnpu_cmd_submit *hdr_out,
				       size_t hdr_out_sz);

/**
 * amdrnpu_syncobj_wait_complete - wait for or probe a submission's completion.
 *
 * @device_fd:		reserved; the completion handle carries the state, so the device
 *		fd is not needed and this argument is ignored.
 * @syncobj_handle: the sync_file fd returned in amdrnpu_cmd_submit()'s
 *		@hdr_out->sync_handle.
 * @timeout_ns:	== 0 non-blocking probe; > 0 waits up to that many relative
 *		nanoseconds (millisecond granularity); < 0 is rejected (-EINVAL).
 * @out_wait_status: 0 not signalled, 1 completed without error, or a negative
 *		errno when signalled with a fence error (e.g. -EREMOTEIO).
 *
 * Unlike the old DRM implementation this does not destroy the handle; the
 * caller still owns @syncobj_handle and must close(2) it.  Returns 0 when the
 * status was written (including a still-pending probe), -ETIMEDOUT if the
 * deadline expired while pending, or a negative errno on failure.
 */
AMDRNPU_LIB_API int amdrnpu_syncobj_wait_complete(int device_fd,
						  uint32_t syncobj_handle,
						  int64_t timeout_ns,
						  int32_t *out_wait_status);

/**
 * amdrnpu_syncobj_fence_status - non-blocking completion status query.
 *
 * @device_fd:		reserved; ignored (see amdrnpu_syncobj_wait_complete()).
 * @syncobj_handle: the sync_file fd from amdrnpu_cmd_submit().
 * @out_fence_status: >0 signalled, 0 pending, <0 fence errno.
 *
 * Does not close the handle.  Safe in a polling loop.  Returns 0 on a
 * successful query or a negative errno on failure.
 */
AMDRNPU_LIB_API int amdrnpu_syncobj_fence_status(int device_fd,
						 uint32_t syncobj_handle,
						 int32_t *out_fence_status);

/**
 * amdrnpu_syncobj_wait_busy_poll - OBSOLETE; delegates to wait_complete().
 *
 * No way to query command status ahead of the response message yet, so there
 * is nothing to busy-poll; prefer amdrnpu_syncobj_wait_complete() directly.
 *
 * @device_fd:		reserved; ignored (see amdrnpu_syncobj_wait_complete()).
 * @syncobj_handle: the sync_file fd from amdrnpu_cmd_submit().
 * @max_wall_ns: forwarded as amdrnpu_syncobj_wait_complete()'s @timeout_ns.
 * @interval_ns: reserved; ignored (no dedicated busy-poll path yet).
 * @out_fence_status: same semantics as amdrnpu_syncobj_fence_status().
 *
 * Does not close the handle.  Returns 0 on success or a negative errno.
 */
AMDRNPU_LIB_API int amdrnpu_syncobj_wait_busy_poll(int device_fd,
						   uint32_t syncobj_handle,
						   int64_t max_wall_ns,
						   uint64_t interval_ns,
						   int32_t *out_fence_status);

/**
 * amdrnpu_syncobj_to_fence_fd - obtain a poll(2)-able fence fd for a submission.
 *
 * @device_fd:		reserved; ignored (see amdrnpu_syncobj_wait_complete()).
 * @syncobj_handle: the sync_file fd from amdrnpu_cmd_submit().
 * @out_fence_fd: on success, a new fd (dup, O_CLOEXEC) the caller must close.
 *
 * The submission handle is already a sync_file fd; this returns an independent
 * duplicate suitable for a blocking poll(2).  Returns 0 on success or a
 * negative errno.
 */
AMDRNPU_LIB_API int amdrnpu_syncobj_to_fence_fd(int device_fd,
						uint32_t syncobj_handle,
						int *out_fence_fd);

#ifdef __cplusplus
}
#endif
#endif /* AMDRNPU_LIB_H_ */
