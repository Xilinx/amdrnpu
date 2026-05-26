/* SPDX-License-Identifier: MIT */
/*
 * tessai_agent_lib - userspace helpers for the tessai-agent DRM driver.
 *
 * Stable ABI and ioctl layout: `<tessai-agent/tessai_agent.h>`.
 *
 * Buffer lifecycle: **`tessai_agent_buf_alloc()`** /
 * **`tessai_agent_buf_close()`** (`DRM_IOCTL_GEM_CLOSE`).
 * Cacheable native buffers require **`tessai_agent_buf_sync()`** (or the
 * direction helpers) around device DMA — see buffer-management.md.
 *
 * Command completion: **`tessai_agent_syncobj_wait_complete()`** and
 * **`tessai_agent_syncobj_wait_busy_poll()`** both call **`DRM_IOCTL_SYNCOBJ_DESTROY`**
 * on the submit syncobj after the kernel reports a **non-pending** fence (success,
 * error class from firmware, or driver completion). Pending probes leave the handle
 * valid. **`tessai_agent_syncobj_fence_status()`** never destroys (intended for raw
 * polling loops). **`tessai_agent_syncobj_to_fence_fd()`** exports for `poll(2)`.
 */
#ifndef TESSAI_AGENT_LIB_H_
#define TESSAI_AGENT_LIB_H_

#include <stddef.h>
#include <stdint.h>

#include <tessai-agent/tessai_agent.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
#  define TESSAI_AGENT_LIB_API
#elif defined(__GNUC__) && __GNUC__ >= 4
#  define TESSAI_AGENT_LIB_API __attribute__((visibility("default")))
#else
#  define TESSAI_AGENT_LIB_API
#endif

/**
 * tessai_agent_buf_alloc - TESSAI_BUF_ALLOC ioctl wrapper.
 *
 * Returns 0 on success; sets @out_handle to the DRM GEM handle.
 */
TESSAI_AGENT_LIB_API int tessai_agent_buf_alloc(int drm_fd, uint32_t memory_type,
						  uint64_t size, uint32_t *out_handle);

/**
 * tessai_agent_buf_close - DRM_IOCTL_GEM_CLOSE (@handle).
 *
 * Drops the handle reference; the backing buffer may remain alive if a submitted
 * command still pins it — see buffer-management.md §6.
 */
TESSAI_AGENT_LIB_API int tessai_agent_buf_close(int drm_fd, uint32_t handle);

/**
 * tessai_agent_buf_mmap - map a page-aligned range of a GEM buffer.
 *
 * Uses DRM_IOCTL_MODE_MAP_DUMB + mmap(). @offset and @length must be
 * page-aligned. @length 0 maps from @offset through end-of-buffer and
 * requires @buf_size (the allocation size from tessai_agent_buf_alloc).
 *
 * Partial mmap is supported: only the requested page range is mapped into
 * the process (the driver honours mmap offset within the BO).
 *
 * @drm_fd must be a DRM **card** node (/dev/dri/cardN), not renderD*:
 * MODE_MAP_DUMB is not permitted on render clients in DRM core.
 *
 * On success sets @out_map to the mapping base; unmap with
 * tessai_agent_buf_munmap(@out_map, mapped_length).
 */
TESSAI_AGENT_LIB_API int tessai_agent_buf_mmap(int drm_fd, uint32_t handle,
						 uint64_t buf_size,
						 uint64_t offset, uint64_t length,
						 void **out_map);

/** munmap() wrapper; @length must match the length passed to buf_mmap. */
TESSAI_AGENT_LIB_API void tessai_agent_buf_munmap(void *addr, uint64_t length);

/**
 * tessai_agent_buf_sync - TESSAI_BUF_SYNC for a native driver-allocated buffer.
 *
 * @direction: TESSAI_BUF_SYNC_TO_DEVICE or TESSAI_BUF_SYNC_FROM_DEVICE.
 * @offset: byte offset into the buffer (0 = start).
 * @length: bytes to sync; 0 means from @offset through end-of-buffer.
 *
 * Returns -EOPNOTSUPP for PRIME-imported handles (use the exporter's sync).
 */
TESSAI_AGENT_LIB_API int tessai_agent_buf_sync(int drm_fd, uint32_t handle,
						 uint32_t direction,
						 uint64_t offset,
						 uint64_t length);

/** Flush a CPU-written range so device DMA sees it. */
static inline int tessai_agent_buf_sync_to_device(int drm_fd, uint32_t handle,
						   uint64_t offset,
						   uint64_t length)
{
	return tessai_agent_buf_sync(drm_fd, handle,
				     TESSAI_BUF_SYNC_TO_DEVICE, offset, length);
}

/** Invalidate a range after device DMA before the CPU reads it. */
static inline int tessai_agent_buf_sync_from_device(int drm_fd, uint32_t handle,
						     uint64_t offset,
						     uint64_t length)
{
	return tessai_agent_buf_sync(drm_fd, handle,
				     TESSAI_BUF_SYNC_FROM_DEVICE, offset, length);
}

/**
 * tessai_agent_cmd_submit - TESSAI_CMD_SUBMIT; syncobj handle in @hdr_out->drm_sync_obj_handle on success.
 *
 * Builds a variable-length ioctl buffer from @command_op and @args[0 .. @num_args-1].
 * See include/uapi/tessai-agent/tessai_agent.h for a full example (e.g. OP_CODE_CMD0
 * with 16 MiB input BO + 8 MiB output BO via six tessai_arg entries).
 */
TESSAI_AGENT_LIB_API int tessai_agent_cmd_submit(int drm_fd,
						   uint32_t command_op,
						   const struct tessai_arg *args,
						   uint32_t num_args,
						   struct tessai_cmd_submit *hdr_out,
						   size_t hdr_out_sz);

/**
 * tessai_agent_syncobj_wait_complete - wait or probe syncobj-backed command.
 *
 * @timeout_ns == 0: non-blocking probe via exported sync_file + `SYNC_IOC_FILE_INFO`.
 * @timeout_ns > 0: waits up to @timeout_ns relative nanoseconds (`DRM_SYNCOBJ_WAIT`),
 *	then refreshes fence status via the same probe path.
 * @timeout_ns < 0 is rejected with `-EINVAL`.
 *
 * Writes @out_wait_status: **0** not signaled, **1** completed without fence error,
 * negative errno (`dma_fence_get_status`) when signaled with error (e.g. `-EREMOTEIO`,
 * `-ECANCELED`, `-EOPNOTSUPP` from remote completion mapping).
 *
 * After a **non-zero** @out_wait_status (fence reached a terminal state from the
 * driver), the library issues **`DRM_IOCTL_SYNCOBJ_DESTROY`** on @syncobj_handle
 * (same teardown rule as **`tessai_agent_syncobj_wait_busy_poll()`**). While the
 * status remains **0** (pending), the handle is left intact.
 *
 * Return value **0**: wait/query succeeded and status was written (including probe
 * of a still-pending fence). **-ETIMEDOUT**: deadline expired while still pending.
 * Other negatives: ioctl/query failure (`-errno`).
 */
TESSAI_AGENT_LIB_API int tessai_agent_syncobj_wait_complete(int drm_fd,
							       uint32_t syncobj_handle,
							       int64_t timeout_ns,
							       int32_t *out_wait_status);

/**
 * tessai_agent_syncobj_fence_status - exported sync_file + SYNC_IOC_FILE_INFO.
 *
 * **Non-blocking.** Does **not** destroy the syncobj. Safe to call in a polling loop
 * (`out_fence_status == 0` ⇒ fence still pending).
 *
 *   >0 — signaled (typically `1`),
 *    0 — still pending,
 *   <0 — dma_fence errno (negative, e.g. `-EREMOTEIO`).
 *
 * Returns 0 on success of the query; negative errno on failure to export or ioctl.
 */
TESSAI_AGENT_LIB_API int tessai_agent_syncobj_fence_status(int drm_fd,
							     uint32_t syncobj_handle,
							     int32_t *out_fence_status);

/**
 * tessai_agent_syncobj_wait_busy_poll - poll fence status until signaled or timed out.
 *
 * Each iteration invokes `tessai_agent_syncobj_fence_status()`. When the fence
 * leaves the pending state (`*out_fence_status != 0`), issues
 * **`DRM_IOCTL_SYNCOBJ_DESTROY`** on @syncobj_handle (same as **`…_wait_complete()`**
 * on terminal completion).
 *
 * @max_wall_ns: monotonic **relative** ceiling: `< 0` = wait indefinitely;
 *               `≥ 0` = return `-ETIMEDOUT` once elapsed wall time exceeds this
 *                       after still pending (`0` ⇒ single probe-like attempt).
 * @interval_ns: `> 0` sleep between probes; `== 0` use `sched_yield()` only (no CPU
 *               sleep — tight cooperative polling, avoid on large multicore workloads).
 *
 * On success writes the same semantics as fence_status (`1`/`>0`, `0`, or errno).
 */
TESSAI_AGENT_LIB_API int tessai_agent_syncobj_wait_busy_poll(int drm_fd,
							       uint32_t syncobj_handle,
							       int64_t max_wall_ns,
							       uint64_t interval_ns,
							       int32_t *out_fence_status);

/**
 * tessai_agent_syncobj_to_fence_fd - export one-shot sync_file for blocking poll(2).
 *
 * Does **not** destroy the submit syncobj; caller must still complete via
 * **`tessai_agent_syncobj_wait_complete()`** / **`…_wait_busy_poll()`**, or destroy
 * manually if using only fence export.
 */
TESSAI_AGENT_LIB_API int tessai_agent_syncobj_to_fence_fd(int drm_fd,
							    uint32_t syncobj_handle,
							    int *out_fence_fd);

#ifdef __cplusplus
}
#endif
#endif /* TESSAI_AGENT_LIB_H_ */
