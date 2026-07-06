/* SPDX-License-Identifier: MIT */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <libdrm/drm.h>
#include <linux/sync_file.h>

#include <tessai_agent/tessai_agent_lib.h>

#include "tessai_agent_trace.h"

#ifndef DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE
#define DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE (1 << 0)
#endif

/*
 * Must match linux UAPI drm_syncobj_wait (incl. flags / deadline extensions).
 */
struct tessai_drm_syncobj_wait {
	uint64_t handles;
	int64_t timeout_nsec;
	uint32_t count_handles;
	uint32_t flags;
	uint32_t first_signaled;
	uint32_t pad;
	uint64_t deadline_nsec;
};

static int syncobj_wait_rel(int drm_fd, uint32_t syncobj_handle,
			    const int64_t *timeout_ns)
{
	struct tessai_drm_syncobj_wait wait;
	uint32_t handles[1] = { syncobj_handle };

	memset(&wait, 0, sizeof(wait));
	wait.handles = (uintptr_t)handles;
	wait.count_handles = 1;
	wait.flags = 0;
	wait.first_signaled = 0;
	wait.pad = 0;
	wait.deadline_nsec = 0;

	if (!timeout_ns || *timeout_ns < 0) {
		wait.timeout_nsec = INT64_MAX;
	} else {
		struct timespec ts;

		if (clock_gettime(CLOCK_MONOTONIC, &ts))
			return -errno;

		int64_t now = (int64_t)ts.tv_sec * INT64_C(1000000000)
			      + ts.tv_nsec;
		int64_t rel = *timeout_ns;

		if (rel > INT64_MAX - now)
			wait.timeout_nsec = INT64_MAX;
		else
			wait.timeout_nsec = now + rel;
	}

	TESSAI_TRACE1(wait_ioctl_enter, syncobj_handle);
	if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait)) {
		int e = -errno;

		TESSAI_TRACE1(wait_ioctl_exit, e);
		return e;
	}
	TESSAI_TRACE1(wait_ioctl_exit, 0);
	return 0;
}

/*
 * Driver-terminal fences: drop the userspace syncobj handle so the per-submit
 * object does not linger (matches wait_busy_poll behaviour).
 */
static void syncobj_destroy_if_terminal(int drm_fd, uint32_t syncobj_handle,
					int32_t fence_status)
{
	struct drm_syncobj_destroy arg;

	if (fence_status == 0)
		return;
	memset(&arg, 0, sizeof(arg));
	arg.handle = syncobj_handle;
	(void)ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &arg);
}

int tessai_agent_buf_alloc(int drm_fd, uint32_t memory_type, uint64_t size,
			    uint32_t *out_handle)
{
	struct tessai_buf_alloc a = {
		.memory_type	= memory_type,
		.size		= size,
	};

	TESSAI_TRACE2(buf_alloc_enter, memory_type, size);
	if (!out_handle) {
		TESSAI_TRACE2(buf_alloc_exit, 0, -EINVAL);
		return -EINVAL;
	}
	if (ioctl(drm_fd, TESSAI_BUF_ALLOC, &a)) {
		int e = -errno;

		TESSAI_TRACE2(buf_alloc_exit, 0, e);
		return e;
	}
	*out_handle = a.drm_handle;
	TESSAI_TRACE2(buf_alloc_exit, a.drm_handle, 0);
	return 0;
}

int tessai_agent_buf_close(int drm_fd, uint32_t handle)
{
	struct drm_gem_close arg = { .handle = handle };

	TESSAI_TRACE1(buf_close_enter, handle);
	if (ioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &arg)) {
		int e = -errno;

		TESSAI_TRACE1(buf_close_exit, e);
		return e;
	}
	TESSAI_TRACE1(buf_close_exit, 0);
	return 0;
}

static unsigned long tessai_page_size(void)
{
	long psz = sysconf(_SC_PAGESIZE);

	if (psz <= 0)
		return 4096UL;
	return (unsigned long)psz;
}

static bool tessai_page_aligned(uint64_t val, unsigned long psz)
{
	return (val & (psz - 1)) == 0;
}

int tessai_agent_buf_mmap(int drm_fd, uint32_t handle, uint64_t buf_size,
			  uint64_t offset, uint64_t length, void **out_map)
{
	struct drm_mode_map_dumb md = { .handle = handle };
	unsigned long psz = tessai_page_size();
	void *map;

	TESSAI_TRACE1(buf_mmap_enter, handle);

	if (!out_map)
		TESSAI_TRACE_RET(buf_mmap_exit, -EINVAL);

	if (!tessai_page_aligned(offset, psz))
		TESSAI_TRACE_RET(buf_mmap_exit, -EINVAL);
	if (length && !tessai_page_aligned(length, psz))
		TESSAI_TRACE_RET(buf_mmap_exit, -EINVAL);

	if (!length) {
		if (!buf_size || offset > buf_size)
			TESSAI_TRACE_RET(buf_mmap_exit, -EINVAL);
		length = buf_size - offset;
	} else if (buf_size && offset + length > buf_size) {
		TESSAI_TRACE_RET(buf_mmap_exit, -EINVAL);
	}

	if (!length)
		TESSAI_TRACE_RET(buf_mmap_exit, -EINVAL);

	if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md))
		TESSAI_TRACE_RET(buf_mmap_exit, -errno);

	map = mmap(NULL, (size_t)length, PROT_READ | PROT_WRITE, MAP_SHARED,
		   drm_fd, (off_t)(md.offset + offset));
	if (map == MAP_FAILED)
		TESSAI_TRACE_RET(buf_mmap_exit, -errno);

	*out_map = map;
	TESSAI_TRACE_RET(buf_mmap_exit, 0);
}

void tessai_agent_buf_munmap(void *addr, uint64_t length)
{
	TESSAI_TRACE0(buf_munmap_enter);
	if (!addr || !length) {
		TESSAI_TRACE0(buf_munmap_exit);
		return;
	}
	(void)munmap(addr, (size_t)length);
	TESSAI_TRACE0(buf_munmap_exit);
}

int tessai_agent_buf_sync(int drm_fd, uint32_t handle, uint32_t direction,
			  uint64_t offset, uint64_t length)
{
	struct tessai_buf_sync arg = {
		.drm_handle	= handle,
		.direction	= direction,
		.offset		= offset,
		.length		= length,
	};

	TESSAI_TRACE2(buf_sync_enter, handle, direction);
	if (ioctl(drm_fd, TESSAI_BUF_SYNC, &arg))
		TESSAI_TRACE_RET(buf_sync_exit, -errno);
	TESSAI_TRACE_RET(buf_sync_exit, 0);
}

int tessai_agent_cmd_submit(int drm_fd, uint32_t command_op,
			     const struct tessai_arg *args, uint32_t num_args,
			     struct tessai_cmd_submit *hdr_out, size_t hdr_out_sz)
{
	size_t fix = offsetof(struct tessai_cmd_submit, args);
	size_t need = fix + (size_t)num_args * sizeof(struct tessai_arg);
	unsigned char *buf;
	struct tessai_cmd_submit *cmd;
	const size_t copy_sz = fix;

	TESSAI_TRACE2(cmd_submit_enter, command_op, num_args);
	if (!hdr_out || hdr_out_sz < copy_sz)
		TESSAI_TRACE_RET(cmd_submit_exit, -EINVAL);
	if (num_args && !args)
		TESSAI_TRACE_RET(cmd_submit_exit, -EINVAL);

	buf = calloc(1, need);
	if (!buf)
		TESSAI_TRACE_RET(cmd_submit_exit, -ENOMEM);

	cmd = (struct tessai_cmd_submit *)buf;
	cmd->drm_sync_obj_handle = 0;
	cmd->command_op = command_op;
	cmd->num_args = num_args;
	if (num_args)
		memcpy(cmd->args, args, (size_t)num_args * sizeof(struct tessai_arg));

	if (ioctl(drm_fd, TESSAI_CMD_SUBMIT, buf)) {
		int e = -errno;

		free(buf);
		TESSAI_TRACE_RET(cmd_submit_exit, e);
	}

	memcpy(hdr_out, cmd, copy_sz);
	free(buf);
	TESSAI_TRACE_RET(cmd_submit_exit, 0);
}

int tessai_agent_syncobj_wait_complete(int drm_fd, uint32_t syncobj_handle,
					int64_t timeout_ns,
					int32_t *out_wait_status)
{
	int err;
	int32_t st;

	TESSAI_TRACE2(wait_complete_enter, syncobj_handle, timeout_ns);
	if (!out_wait_status)
		TESSAI_TRACE_RET(wait_complete_exit, -EINVAL);
	if (timeout_ns < 0)
		TESSAI_TRACE_RET(wait_complete_exit, -EINVAL);

	if (timeout_ns == 0)
		err = tessai_agent_syncobj_fence_status(drm_fd, syncobj_handle,
							 &st);
	else
		err = 0;

	if (timeout_ns != 0) {
		err = syncobj_wait_rel(drm_fd, syncobj_handle, &timeout_ns);
		if (err) {
			if (err == -ETIME || err == -ETIMEDOUT) {
				err = tessai_agent_syncobj_fence_status(drm_fd,
									 syncobj_handle,
									 &st);
				if (err)
					TESSAI_TRACE_RET(wait_complete_exit, err);
				if (st != 0)
					*out_wait_status = st;
				else
					*out_wait_status = 0;
				syncobj_destroy_if_terminal(drm_fd, syncobj_handle, st);
				TESSAI_TRACE_RET(wait_complete_exit, -ETIMEDOUT);
			}
			TESSAI_TRACE_RET(wait_complete_exit, err);
		}
		err = tessai_agent_syncobj_fence_status(drm_fd, syncobj_handle,
							 &st);
	}

	if (err)
		TESSAI_TRACE_RET(wait_complete_exit, err);
	*out_wait_status = st;
	syncobj_destroy_if_terminal(drm_fd, syncobj_handle, st);
	TESSAI_TRACE_RET(wait_complete_exit, 0);
}

static void tessai_rel_sleep_ns(uint64_t ns)
{
	struct timespec rq;

	if (!ns)
		return;
	rq.tv_sec = (time_t)(ns / UINT64_C(1000000000));
	rq.tv_nsec = (long)(ns % UINT64_C(1000000000));

	while (nanosleep(&rq, &rq) == -1 && errno == EINTR)
		;
}

static int64_t tessai_mono_elapsed_ns(const struct timespec *start,
				    const struct timespec *now)
{
	return (int64_t)(now->tv_sec - start->tv_sec) * INT64_C(1000000000)
	       + (int64_t)(now->tv_nsec - start->tv_nsec);
}

int tessai_agent_syncobj_to_fence_fd(int drm_fd, uint32_t syncobj_handle,
				      int *out_fence_fd)
{
	struct drm_syncobj_handle arg;

	TESSAI_TRACE1(to_fence_fd_enter, syncobj_handle);
	if (!out_fence_fd)
		TESSAI_TRACE_RET(to_fence_fd_exit, -EINVAL);

	memset(&arg, 0, sizeof(arg));
	arg.handle = syncobj_handle;
	arg.flags  = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
	if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &arg))
		TESSAI_TRACE_RET(to_fence_fd_exit, -errno);
	*out_fence_fd = arg.fd;
	TESSAI_TRACE_RET(to_fence_fd_exit, 0);
}

int tessai_agent_syncobj_fence_status(int drm_fd, uint32_t syncobj_handle,
				       int32_t *out_fence_status)
{
	struct sync_file_info fi;
	int sf, q;

	TESSAI_TRACE1(fence_status_enter, syncobj_handle);
	if (!out_fence_status)
		TESSAI_TRACE_RET(fence_status_exit, -EINVAL);

	q = tessai_agent_syncobj_to_fence_fd(drm_fd, syncobj_handle, &sf);
	if (q < 0)
		TESSAI_TRACE_RET(fence_status_exit, q);

	memset(&fi, 0, sizeof(fi));
	if (ioctl(sf, SYNC_IOC_FILE_INFO, &fi)) {
		q = -errno;
		close(sf);
		TESSAI_TRACE_RET(fence_status_exit, q);
	}
	close(sf);
	*out_fence_status = fi.status;
	TESSAI_TRACE_RET(fence_status_exit, 0);
}

int tessai_agent_syncobj_wait_busy_poll(int drm_fd, uint32_t syncobj_handle,
					 int64_t max_wall_ns,
					 uint64_t interval_ns,
					 int32_t *out_fence_status)
{
	struct timespec t0;
	bool use_deadline;

	TESSAI_TRACE1(wait_busy_poll_enter, syncobj_handle);
	if (!out_fence_status)
		TESSAI_TRACE_RET(wait_busy_poll_exit, -EINVAL);

	use_deadline = max_wall_ns >= 0;

	if (clock_gettime(CLOCK_MONOTONIC, &t0))
		TESSAI_TRACE_RET(wait_busy_poll_exit, -errno);

	for (;;) {
		int err;
		int32_t st;

		err = tessai_agent_syncobj_fence_status(drm_fd, syncobj_handle,
							 &st);
		if (err)
			TESSAI_TRACE_RET(wait_busy_poll_exit, err);
		*out_fence_status = st;
		if (st != 0) {
			syncobj_destroy_if_terminal(drm_fd, syncobj_handle, st);
			TESSAI_TRACE_RET(wait_busy_poll_exit, 0);
		}

		if (use_deadline) {
			struct timespec now;
			int64_t elapsed;

			if (clock_gettime(CLOCK_MONOTONIC, &now))
				TESSAI_TRACE_RET(wait_busy_poll_exit, -errno);
			elapsed = tessai_mono_elapsed_ns(&t0, &now);
			if (elapsed >= max_wall_ns)
				TESSAI_TRACE_RET(wait_busy_poll_exit, -ETIMEDOUT);
		}

		if (interval_ns > 0)
			tessai_rel_sleep_ns(interval_ns);
		else
			sched_yield();
	}
}
