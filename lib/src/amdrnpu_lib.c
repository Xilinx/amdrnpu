/* SPDX-License-Identifier: MIT */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/sync_file.h>

#include <amdrnpu/amdrnpu_lib.h>

#include "amdrnpu_trace.h"

/* ------------------------------------------------------------------ */
/* Internal handle registry                                           */
/*                                                                    */
/* amdrnpu_buf_alloc() returns the kernel's per-file import handle, but */
/* mmap()/DMA_BUF_IOCTL_SYNC operate on the dma-buf fd.  The old API    */
/* exposed only the handle, so the library remembers the dma-buf fd and */
/* size for each (device fd, handle) pair here.                         */
/* ------------------------------------------------------------------ */

struct amdrnpu_buf_entry {
	int		used;
	int		dev_fd;
	uint32_t	handle;
	int		dmabuf_fd;
	uint64_t	size;
};

static pthread_mutex_t amdrnpu_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static struct amdrnpu_buf_entry *amdrnpu_reg;
static size_t amdrnpu_reg_cap;

static int amdrnpu_reg_add(int dev_fd, uint32_t handle, int dmabuf_fd,
			   uint64_t size)
{
	size_t i, slot = (size_t)-1;

	pthread_mutex_lock(&amdrnpu_reg_lock);
	for (i = 0; i < amdrnpu_reg_cap; i++) {
		if (!amdrnpu_reg[i].used) {
			slot = i;
			break;
		}
	}
	if (slot == (size_t)-1) {
		size_t ncap = amdrnpu_reg_cap ? amdrnpu_reg_cap * 2 : 8;
		struct amdrnpu_buf_entry *n =
			realloc(amdrnpu_reg, ncap * sizeof(*n));

		if (!n) {
			pthread_mutex_unlock(&amdrnpu_reg_lock);
			return -ENOMEM;
		}
		memset(n + amdrnpu_reg_cap, 0,
		       (ncap - amdrnpu_reg_cap) * sizeof(*n));
		slot = amdrnpu_reg_cap;
		amdrnpu_reg = n;
		amdrnpu_reg_cap = ncap;
	}

	amdrnpu_reg[slot].used = 1;
	amdrnpu_reg[slot].dev_fd = dev_fd;
	amdrnpu_reg[slot].handle = handle;
	amdrnpu_reg[slot].dmabuf_fd = dmabuf_fd;
	amdrnpu_reg[slot].size = size;
	pthread_mutex_unlock(&amdrnpu_reg_lock);
	return 0;
}

/* Look up an entry (leaving it in place). Returns 1 if found, else 0. */
static int amdrnpu_reg_lookup(int dev_fd, uint32_t handle, int *out_dmabuf_fd,
			      uint64_t *out_size)
{
	int found = 0;
	size_t i;

	pthread_mutex_lock(&amdrnpu_reg_lock);
	for (i = 0; i < amdrnpu_reg_cap; i++) {
		if (amdrnpu_reg[i].used &&
		    amdrnpu_reg[i].dev_fd == dev_fd &&
		    amdrnpu_reg[i].handle == handle) {
			if (out_dmabuf_fd)
				*out_dmabuf_fd = amdrnpu_reg[i].dmabuf_fd;
			if (out_size)
				*out_size = amdrnpu_reg[i].size;
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&amdrnpu_reg_lock);
	return found;
}

/* Look up and remove an entry. Returns 1 if found, else 0. */
static int amdrnpu_reg_take(int dev_fd, uint32_t handle, int *out_dmabuf_fd,
			    uint64_t *out_size)
{
	int found = 0;
	size_t i;

	pthread_mutex_lock(&amdrnpu_reg_lock);
	for (i = 0; i < amdrnpu_reg_cap; i++) {
		if (amdrnpu_reg[i].used &&
		    amdrnpu_reg[i].dev_fd == dev_fd &&
		    amdrnpu_reg[i].handle == handle) {
			if (out_dmabuf_fd)
				*out_dmabuf_fd = amdrnpu_reg[i].dmabuf_fd;
			if (out_size)
				*out_size = amdrnpu_reg[i].size;
			amdrnpu_reg[i].used = 0;
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&amdrnpu_reg_lock);
	return found;
}

/* ------------------------------------------------------------------ */
/* Internal char-driver primitives                                    */
/* ------------------------------------------------------------------ */

/* Map memory_type -> dma-buf heap name (env-overridable) and import flag. */
static const char *amdrnpu_heap_for_memtype(uint32_t memory_type)
{
	const char *env;

	if (memory_type == AMDRNPU_MEMORY_RPU) {
		env = getenv("AMDRNPU_HEAP_RPU");
		return env ? env : "reserved";
	}
	env = getenv("AMDRNPU_HEAP_DEVICE");
	return env ? env : "reserved";
}

static uint32_t amdrnpu_flag_for_memtype(uint32_t memory_type)
{
	return (memory_type == AMDRNPU_MEMORY_RPU) ? AMDRNPU_BUF_IMPORT_FW
						   : AMDRNPU_BUF_IMPORT_DEVICE;
}

/* Allocate a physically-contiguous dma-buf from a heap. */
static int amdrnpu_heap_alloc(const char *heap, uint64_t size, int *out_fd)
{
	struct dma_heap_allocation_data data;
	char path[128];
	int heap_fd;

	if (strchr(heap, '/')) {
		if ((size_t)snprintf(path, sizeof(path), "%s", heap) >=
		    sizeof(path))
			return -ENAMETOOLONG;
	} else {
		if ((size_t)snprintf(path, sizeof(path), "/dev/dma_heap/%s",
				     heap) >= sizeof(path))
			return -ENAMETOOLONG;
	}

	heap_fd = open(path, O_RDWR | O_CLOEXEC);
	if (heap_fd < 0)
		return -errno;

	memset(&data, 0, sizeof(data));
	data.len = size;
	data.fd_flags = O_RDWR | O_CLOEXEC;
	if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data)) {
		int e = -errno;

		close(heap_fd);
		return e;
	}
	close(heap_fd);

	*out_fd = (int)data.fd;
	return 0;
}

static int amdrnpu_import(int fd, int dmabuf_fd, uint32_t flags,
			  uint32_t *out_handle)
{
	struct amdrnpu_buf_import arg;

	memset(&arg, 0, sizeof(arg));
	arg.fd = dmabuf_fd;
	arg.flags = flags;

	if (ioctl(fd, AMDRNPU_BUF_IMPORT, &arg))
		return -errno;

	*out_handle = arg.handle;
	return 0;
}

static int amdrnpu_unimport(int fd, uint32_t handle)
{
	struct amdrnpu_buf_destroy arg;

	memset(&arg, 0, sizeof(arg));
	arg.handle = handle;

	if (ioctl(fd, AMDRNPU_BUF_DESTROY, &arg))
		return -errno;
	return 0;
}

/* SYNC_IOC_FILE_INFO status: >0 signalled, 0 pending, <0 fence errno. */
static int amdrnpu_fence_status(int fence_fd, int32_t *out_status)
{
	struct sync_file_info info;

	if (fence_fd < 0)
		return -EINVAL;

	memset(&info, 0, sizeof(info));
	if (ioctl(fence_fd, SYNC_IOC_FILE_INFO, &info))
		return -errno;

	*out_status = info.status;
	return 0;
}

static int amdrnpu_poll_fence(int fence_fd, int timeout_ms)
{
	struct pollfd pfd = {
		.fd	= fence_fd,
		.events	= POLLIN,
	};
	int ret;

	do {
		ret = poll(&pfd, 1, timeout_ms);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0)
		return -errno;
	if (ret == 0)
		return -ETIMEDOUT;
	if (pfd.revents & (POLLERR | POLLNVAL))
		return -EIO;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int amdrnpu_buf_alloc(int fd, uint32_t memory_type, uint64_t size,
		      uint32_t *out_handle)
{
	int dmabuf_fd = -1;
	uint32_t handle = 0;
	const char *heap;
	uint32_t flags;
	int ret;

	AMDRNPU_TRACE2(buf_alloc_enter, memory_type, (int)size);
	if (!out_handle || size == 0)
		AMDRNPU_TRACE_RET(buf_alloc_exit, -EINVAL);
	if (memory_type != AMDRNPU_MEMORY_RPU &&
	    memory_type != AMDRNPU_MEMORY_DEVICE)
		AMDRNPU_TRACE_RET(buf_alloc_exit, -EINVAL);

	heap = amdrnpu_heap_for_memtype(memory_type);
	flags = amdrnpu_flag_for_memtype(memory_type);

	ret = amdrnpu_heap_alloc(heap, size, &dmabuf_fd);
	if (ret)
		AMDRNPU_TRACE_RET(buf_alloc_exit, ret);

	ret = amdrnpu_import(fd, dmabuf_fd, flags, &handle);
	if (ret) {
		close(dmabuf_fd);
		AMDRNPU_TRACE_RET(buf_alloc_exit, ret);
	}

	ret = amdrnpu_reg_add(fd, handle, dmabuf_fd, size);
	if (ret) {
		amdrnpu_unimport(fd, handle);
		close(dmabuf_fd);
		AMDRNPU_TRACE_RET(buf_alloc_exit, ret);
	}

	*out_handle = handle;
	AMDRNPU_TRACE_RET(buf_alloc_exit, 0);
}

int amdrnpu_buf_close(int fd, uint32_t handle)
{
	int dmabuf_fd = -1;
	int ret;

	AMDRNPU_TRACE1(buf_close_enter, handle);
	if (!amdrnpu_reg_take(fd, handle, &dmabuf_fd, NULL))
		AMDRNPU_TRACE_RET(buf_close_exit, -ENOENT);

	ret = amdrnpu_unimport(fd, handle);
	close(dmabuf_fd);
	AMDRNPU_TRACE_RET(buf_close_exit, ret);
}

int amdrnpu_buf_get_phys(int fd, uint32_t handle, uint64_t *out_pa,
			 uint64_t *out_size)
{
	struct amdrnpu_buf_query arg = {
		.handle = handle,
	};
	int ret;

	if (!out_pa)
		return -EINVAL;
	if (!amdrnpu_reg_lookup(fd, handle, NULL, NULL))
		return -ENOENT;

	ret = ioctl(fd, AMDRNPU_BUF_QUERY, &arg);
	if (ret)
		return -errno;

	*out_pa = arg.dma_addr;
	if (out_size)
		*out_size = arg.size;
	return 0;
}

int amdrnpu_buf_mmap(int fd, uint32_t handle, uint64_t buf_size,
		     uint64_t offset, uint64_t length, void **out_map)
{
	int dmabuf_fd = -1;
	uint64_t size = 0;
	size_t maplen;
	void *map;

	(void)buf_size; /* reserved: size is tracked internally */

	AMDRNPU_TRACE1(buf_mmap_enter, handle);
	if (!out_map)
		AMDRNPU_TRACE_RET(buf_mmap_exit, -EINVAL);
	if (!amdrnpu_reg_lookup(fd, handle, &dmabuf_fd, &size))
		AMDRNPU_TRACE_RET(buf_mmap_exit, -ENOENT);
	if (offset > size)
		AMDRNPU_TRACE_RET(buf_mmap_exit, -EINVAL);

	maplen = length ? (size_t)length : (size_t)(size - offset);
	if (maplen == 0 || offset + maplen > size)
		AMDRNPU_TRACE_RET(buf_mmap_exit, -EINVAL);

	map = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED,
		   dmabuf_fd, (off_t)offset);
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

int amdrnpu_buf_sync(int fd, uint32_t handle, uint32_t direction,
		     uint64_t offset, uint64_t length)
{
	struct dma_buf_sync arg;
	int dmabuf_fd = -1;

	(void)offset; /* reserved: DMA_BUF_IOCTL_SYNC is whole-buffer */
	(void)length; /* reserved */

	AMDRNPU_TRACE2(buf_sync_enter, handle, (int)direction);
	if (!amdrnpu_reg_lookup(fd, handle, &dmabuf_fd, NULL))
		AMDRNPU_TRACE_RET(buf_sync_exit, -ENOENT);

	memset(&arg, 0, sizeof(arg));
	if (direction == AMDRNPU_BUF_SYNC_TO_DEVICE)
		arg.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
	else if (direction == AMDRNPU_BUF_SYNC_FROM_DEVICE)
		arg.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
	else
		AMDRNPU_TRACE_RET(buf_sync_exit, -EINVAL);

	if (ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &arg))
		AMDRNPU_TRACE_RET(buf_sync_exit, -errno);
	AMDRNPU_TRACE_RET(buf_sync_exit, 0);
}

int amdrnpu_cmd_submit(int fd, uint32_t command_op,
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
	cmd->sync_handle = 0;
	cmd->command_op = command_op;
	cmd->num_args = num_args;
	if (num_args)
		memcpy(cmd->args, args,
		       (size_t)num_args * sizeof(struct amdrnpu_arg));

	if (ioctl(fd, AMDRNPU_CMD_SUBMIT, buf)) {
		int e = -errno;

		free(buf);
		AMDRNPU_TRACE_RET(cmd_submit_exit, e);
	}

	memcpy(hdr_out, cmd, copy_sz);
	free(buf);
	AMDRNPU_TRACE_RET(cmd_submit_exit, 0);
}

int amdrnpu_syncobj_fence_status(int fd, uint32_t syncobj_handle,
				 int32_t *out_fence_status)
{
	(void)fd; /* reserved: state lives on the sync_file fd */

	AMDRNPU_TRACE1(fence_status_enter, (int)syncobj_handle);
	if (!out_fence_status)
		AMDRNPU_TRACE_RET(fence_status_exit, -EINVAL);

	AMDRNPU_TRACE_RET(fence_status_exit,
			  amdrnpu_fence_status((int)syncobj_handle,
					       out_fence_status));
}

int amdrnpu_syncobj_wait_complete(int fd, uint32_t syncobj_handle,
				  int64_t timeout_ns, int32_t *out_wait_status)
{
	int fence_fd = (int)syncobj_handle;

	(void)fd; /* reserved */

	AMDRNPU_TRACE2(wait_complete_enter, fence_fd, (int)timeout_ns);
	if (!out_wait_status || timeout_ns < 0)
		AMDRNPU_TRACE_RET(wait_complete_exit, -EINVAL);

	if (timeout_ns > 0) {
		/* Round up to at least 1ms so a sub-ms deadline still blocks. */
		int timeout_ms = (int)(timeout_ns / 1000000);

		if (timeout_ms == 0)
			timeout_ms = 1;

		int ret = amdrnpu_poll_fence(fence_fd, timeout_ms);

		if (ret)
			AMDRNPU_TRACE_RET(wait_complete_exit, ret);
	}

	/* Also covers the timeout_ns == 0 non-blocking probe. */
	AMDRNPU_TRACE_RET(wait_complete_exit,
			  amdrnpu_fence_status(fence_fd, out_wait_status));
}

int amdrnpu_syncobj_wait_busy_poll(int fd, uint32_t syncobj_handle,
				   int64_t max_wall_ns, uint64_t interval_ns,
				   int32_t *out_fence_status)
{
	/*
	 * OBSOLETE: no way to query command status ahead of the response
	 * message, so there is nothing to busy-poll.
	 */
	(void)interval_ns; /* reserved: no dedicated busy-poll path yet */

	AMDRNPU_TRACE2(busy_poll_enter, (int)syncobj_handle, (int)interval_ns);
	AMDRNPU_TRACE_RET(busy_poll_exit,
			  amdrnpu_syncobj_wait_complete(fd, syncobj_handle,
							max_wall_ns,
							out_fence_status));
}

int amdrnpu_syncobj_to_fence_fd(int fd, uint32_t syncobj_handle,
				int *out_fence_fd)
{
	int nf;

	(void)fd; /* reserved */

	AMDRNPU_TRACE1(to_fence_fd_enter, (int)syncobj_handle);
	if (!out_fence_fd)
		AMDRNPU_TRACE_RET(to_fence_fd_exit, -EINVAL);

	nf = fcntl((int)syncobj_handle, F_DUPFD_CLOEXEC, 0);
	if (nf < 0)
		AMDRNPU_TRACE_RET(to_fence_fd_exit, -errno);

	*out_fence_fd = nf;
	AMDRNPU_TRACE_RET(to_fence_fd_exit, 0);
}
