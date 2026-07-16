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

#include <amdrnpu/amdrnpu_lib.h>

#include "amdrnpu_trace.h"

#ifndef DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE
#define DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE (1 << 0)
#endif

/*
 * Must match linux UAPI drm_syncobj_wait (incl. flags / deadline extensions).
 */
struct amdrnpu_drm_syncobj_wait {
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
	struct amdrnpu_drm_syncobj_wait wait;
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

	AMDRNPU_TRACE1(wait_ioctl_enter, syncobj_handle);
	if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait)) {
		int e = -errno;

		AMDRNPU_TRACE1(wait_ioctl_exit, e);
		return e;
	}
	AMDRNPU_TRACE1(wait_ioctl_exit, 0);
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

int amdrnpu_buf_alloc(int drm_fd, uint32_t memory_type, uint64_t size,
			    uint32_t *out_handle)
{
	struct amdrnpu_buf_alloc a = {
		.memory_type	= memory_type,
		.size		= size,
	};

	AMDRNPU_TRACE2(buf_alloc_enter, memory_type, size);
	if (!out_handle) {
		AMDRNPU_TRACE2(buf_alloc_exit, 0, -EINVAL);
		return -EINVAL;
	}
	if (ioctl(drm_fd, AMDRNPU_BUF_ALLOC, &a)) {
		int e = -errno;

		AMDRNPU_TRACE2(buf_alloc_exit, 0, e);
		return e;
	}
	*out_handle = a.drm_handle;
	AMDRNPU_TRACE2(buf_alloc_exit, a.drm_handle, 0);
	return 0;
}

int amdrnpu_buf_close(int drm_fd, uint32_t handle)
{
	struct drm_gem_close arg = { .handle = handle };

	AMDRNPU_TRACE1(buf_close_enter, handle);
	if (ioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &arg)) {
		int e = -errno;

		AMDRNPU_TRACE1(buf_close_exit, e);
		return e;
	}
	AMDRNPU_TRACE1(buf_close_exit, 0);
	return 0;
}

static unsigned long amdrnpu_page_size(void)
{
	long psz = sysconf(_SC_PAGESIZE);

	if (psz <= 0)
		return 4096UL;
	return (unsigned long)psz;
}

static bool amdrnpu_page_aligned(uint64_t val, unsigned long psz)
{
	return (val & (psz - 1)) == 0;
}

int amdrnpu_buf_mmap(int drm_fd, uint32_t handle, uint64_t buf_size,
			  uint64_t offset, uint64_t length, void **out_map)
{
	struct drm_mode_map_dumb md = { .handle = handle };
	unsigned long psz = amdrnpu_page_size();
	void *map;

	AMDRNPU_TRACE1(buf_mmap_enter, handle);

	if (!out_map)
		AMDRNPU_TRACE_RET(buf_mmap_exit, -EINVAL);

	if (!amdrnpu_page_aligned(offset, psz))
		AMDRNPU_TRACE_RET(buf_mmap_exit, -EINVAL);
	if (length && !amdrnpu_page_aligned(length, psz))
		AMDRNPU_TRACE_RET(buf_mmap_exit, -EINVAL);

	if (!length) {
		if (!buf_size || offset > buf_size)
			AMDRNPU_TRACE_RET(buf_mmap_exit, -EINVAL);
		length = buf_size - offset;
	} else if (buf_size && offset + length > buf_size) {
		AMDRNPU_TRACE_RET(buf_mmap_exit, -EINVAL);
	}

	if (!length)
		AMDRNPU_TRACE_RET(buf_mmap_exit, -EINVAL);

	if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md))
		AMDRNPU_TRACE_RET(buf_mmap_exit, -errno);

	map = mmap(NULL, (size_t)length, PROT_READ | PROT_WRITE, MAP_SHARED,
		   drm_fd, (off_t)(md.offset + offset));
	if (map == MAP_FAILED)
		AMDRNPU_TRACE_RET(buf_mmap_exit, -errno);

	*out_map = map;
	AMDRNPU_TRACE_RET(buf_mmap_exit, 0);
}

void amdrnpu_buf_munmap(void *addr, uint64_t length)
{
	AMDRNPU_TRACE0(buf_munmap_enter);
	if (!addr || !length) {
		AMDRNPU_TRACE0(buf_munmap_exit);
		return;
	}
	(void)munmap(addr, (size_t)length);
	AMDRNPU_TRACE0(buf_munmap_exit);
}

int amdrnpu_buf_sync(int drm_fd, uint32_t handle, uint32_t direction,
			  uint64_t offset, uint64_t length)
{
	struct amdrnpu_buf_sync arg = {
		.drm_handle	= handle,
		.direction	= direction,
		.offset		= offset,
		.length		= length,
	};

	AMDRNPU_TRACE2(buf_sync_enter, handle, direction);
	if (ioctl(drm_fd, AMDRNPU_BUF_SYNC, &arg))
		AMDRNPU_TRACE_RET(buf_sync_exit, -errno);
	AMDRNPU_TRACE_RET(buf_sync_exit, 0);
}

int amdrnpu_cmd_submit(int drm_fd, uint32_t command_op,
			     const struct amdrnpu_arg *args, uint32_t num_args,
			     struct amdrnpu_cmd_submit *hdr_out, size_t hdr_out_sz)
{
	size_t fix = offsetof(struct amdrnpu_cmd_submit, args);
	size_t need = fix + (size_t)num_args * sizeof(struct amdrnpu_arg);
	unsigned char *buf;
	struct amdrnpu_cmd_submit *cmd;
	const size_t copy_sz = fix;

	AMDRNPU_TRACE2(cmd_submit_enter, command_op, num_args);
	if (!hdr_out || hdr_out_sz < copy_sz)
		AMDRNPU_TRACE_RET(cmd_submit_exit, -EINVAL);
	if (num_args && !args)
		AMDRNPU_TRACE_RET(cmd_submit_exit, -EINVAL);

	buf = calloc(1, need);
	if (!buf)
		AMDRNPU_TRACE_RET(cmd_submit_exit, -ENOMEM);

	cmd = (struct amdrnpu_cmd_submit *)buf;
	cmd->drm_sync_obj_handle = 0;
	cmd->command_op = command_op;
	cmd->num_args = num_args;
	if (num_args)
		memcpy(cmd->args, args, (size_t)num_args * sizeof(struct amdrnpu_arg));

	if (ioctl(drm_fd, AMDRNPU_CMD_SUBMIT, buf)) {
		int e = -errno;

		free(buf);
		AMDRNPU_TRACE_RET(cmd_submit_exit, e);
	}

	memcpy(hdr_out, cmd, copy_sz);
	free(buf);
	AMDRNPU_TRACE_RET(cmd_submit_exit, 0);
}

int amdrnpu_syncobj_wait_complete(int drm_fd, uint32_t syncobj_handle,
					int64_t timeout_ns,
					int32_t *out_wait_status)
{
	int err;
	int32_t st;

	AMDRNPU_TRACE2(wait_complete_enter, syncobj_handle, timeout_ns);
	if (!out_wait_status)
		AMDRNPU_TRACE_RET(wait_complete_exit, -EINVAL);
	if (timeout_ns < 0)
		AMDRNPU_TRACE_RET(wait_complete_exit, -EINVAL);

	if (timeout_ns == 0)
		err = amdrnpu_syncobj_fence_status(drm_fd, syncobj_handle,
							 &st);
	else
		err = 0;

	if (timeout_ns != 0) {
		err = syncobj_wait_rel(drm_fd, syncobj_handle, &timeout_ns);
		if (err) {
			if (err == -ETIME || err == -ETIMEDOUT) {
				err = amdrnpu_syncobj_fence_status(drm_fd,
									 syncobj_handle,
									 &st);
				if (err)
					AMDRNPU_TRACE_RET(wait_complete_exit, err);
				if (st != 0)
					*out_wait_status = st;
				else
					*out_wait_status = 0;
				syncobj_destroy_if_terminal(drm_fd, syncobj_handle, st);
				AMDRNPU_TRACE_RET(wait_complete_exit, -ETIMEDOUT);
			}
			AMDRNPU_TRACE_RET(wait_complete_exit, err);
		}
		err = amdrnpu_syncobj_fence_status(drm_fd, syncobj_handle,
							 &st);
	}

	if (err)
		AMDRNPU_TRACE_RET(wait_complete_exit, err);
	*out_wait_status = st;
	syncobj_destroy_if_terminal(drm_fd, syncobj_handle, st);
	AMDRNPU_TRACE_RET(wait_complete_exit, 0);
}

static void amdrnpu_rel_sleep_ns(uint64_t ns)
{
	struct timespec rq;

	if (!ns)
		return;
	rq.tv_sec = (time_t)(ns / UINT64_C(1000000000));
	rq.tv_nsec = (long)(ns % UINT64_C(1000000000));

	while (nanosleep(&rq, &rq) == -1 && errno == EINTR)
		;
}

static int64_t amdrnpu_mono_elapsed_ns(const struct timespec *start,
				    const struct timespec *now)
{
	return (int64_t)(now->tv_sec - start->tv_sec) * INT64_C(1000000000)
	       + (int64_t)(now->tv_nsec - start->tv_nsec);
}

int amdrnpu_syncobj_to_fence_fd(int drm_fd, uint32_t syncobj_handle,
				      int *out_fence_fd)
{
	struct drm_syncobj_handle arg;

	AMDRNPU_TRACE1(to_fence_fd_enter, syncobj_handle);
	if (!out_fence_fd)
		AMDRNPU_TRACE_RET(to_fence_fd_exit, -EINVAL);

	memset(&arg, 0, sizeof(arg));
	arg.handle = syncobj_handle;
	arg.flags  = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
	if (ioctl(drm_fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &arg))
		AMDRNPU_TRACE_RET(to_fence_fd_exit, -errno);
	*out_fence_fd = arg.fd;
	AMDRNPU_TRACE_RET(to_fence_fd_exit, 0);
}

int amdrnpu_syncobj_fence_status(int drm_fd, uint32_t syncobj_handle,
				       int32_t *out_fence_status)
{
	struct sync_file_info fi;
	int sf, q;

	AMDRNPU_TRACE1(fence_status_enter, syncobj_handle);
	if (!out_fence_status)
		AMDRNPU_TRACE_RET(fence_status_exit, -EINVAL);

	q = amdrnpu_syncobj_to_fence_fd(drm_fd, syncobj_handle, &sf);
	if (q < 0)
		AMDRNPU_TRACE_RET(fence_status_exit, q);

	memset(&fi, 0, sizeof(fi));
	if (ioctl(sf, SYNC_IOC_FILE_INFO, &fi)) {
		q = -errno;
		close(sf);
		AMDRNPU_TRACE_RET(fence_status_exit, q);
	}
	close(sf);
	*out_fence_status = fi.status;
	AMDRNPU_TRACE_RET(fence_status_exit, 0);
}

int amdrnpu_syncobj_wait_busy_poll(int drm_fd, uint32_t syncobj_handle,
					 int64_t max_wall_ns,
					 uint64_t interval_ns,
					 int32_t *out_fence_status)
{
	struct timespec t0;
	bool use_deadline;

	AMDRNPU_TRACE1(wait_busy_poll_enter, syncobj_handle);
	if (!out_fence_status)
		AMDRNPU_TRACE_RET(wait_busy_poll_exit, -EINVAL);

	use_deadline = max_wall_ns >= 0;

	if (clock_gettime(CLOCK_MONOTONIC, &t0))
		AMDRNPU_TRACE_RET(wait_busy_poll_exit, -errno);

	for (;;) {
		int err;
		int32_t st;

		err = amdrnpu_syncobj_fence_status(drm_fd, syncobj_handle,
							 &st);
		if (err)
			AMDRNPU_TRACE_RET(wait_busy_poll_exit, err);
		*out_fence_status = st;
		if (st != 0) {
			syncobj_destroy_if_terminal(drm_fd, syncobj_handle, st);
			AMDRNPU_TRACE_RET(wait_busy_poll_exit, 0);
		}

		if (use_deadline) {
			struct timespec now;
			int64_t elapsed;

			if (clock_gettime(CLOCK_MONOTONIC, &now))
				AMDRNPU_TRACE_RET(wait_busy_poll_exit, -errno);
			elapsed = amdrnpu_mono_elapsed_ns(&t0, &now);
			if (elapsed >= max_wall_ns)
				AMDRNPU_TRACE_RET(wait_busy_poll_exit, -ETIMEDOUT);
		}

		if (interval_ns > 0)
			amdrnpu_rel_sleep_ns(interval_ns);
		else
			sched_yield();
	}
}
