/* SPDX-License-Identifier: MIT */
/*
 * tessai_agent_lib - userspace helpers for the tessai-agent DRM driver.
 *
 * Stable ABI and ioctl layout: `<tessai-agent/tessai_agent.h>`.
 *
 * Buffer lifecycle: **`tessai_agent_buf_alloc()`** /
 * **`tessai_agent_gem_close()`** (`DRM_IOCTL_GEM_CLOSE`).
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
 * tessai_agent_cmd_submit - TESSAI_CMD_SUBMIT; syncobj handle in @hdr_out->drm_sync_obj_handle on success.
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
