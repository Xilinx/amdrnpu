// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.

#include "common.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>

namespace amdrnpu_test {

int open_amdrnpu_device(char *out_path, size_t out_sz)
{
	DIR *dir = ::opendir("/dev");
	if (!dir)
		return -1;

	// miscdevice registers the char node as /dev/amdrnpu.N.
	static const char kPrefix[] = "amdrnpu.";
	const size_t kPrefixLen = sizeof(kPrefix) - 1;

	int found = -1;
	struct dirent *e;
	while ((e = ::readdir(dir)) != nullptr) {
		if (std::strncmp(e->d_name, kPrefix, kPrefixLen) != 0)
			continue;
		if (!std::isdigit(static_cast<unsigned char>(e->d_name[kPrefixLen])))
			continue;

		char path[PATH_MAX];
		std::snprintf(path, sizeof(path), "/dev/%s", e->d_name);

		int device_fd = ::open(path, O_RDWR | O_CLOEXEC);
		if (device_fd < 0)
			continue;

		if (out_path && out_sz)
			std::snprintf(out_path, out_sz, "%s", path);
		found = device_fd;
		break;
	}
	::closedir(dir);
	return found;
}

int probe_fd(int fd)
{
	if (::fcntl(fd, F_GETFD) < 0)
		return -errno;
	return 0;
}

uint64_t now_ns()
{
	struct timespec ts {};
	::clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull +
	       static_cast<uint64_t>(ts.tv_nsec);
}

int do_cmd_submit(int device_fd, uint32_t command_op,
		  const struct amdrnpu_arg *args, uint32_t num_args,
		  uint32_t *out_handle)
{
	struct amdrnpu_cmd_submit hdr {};
	int rc = amdrnpu_cmd_submit(device_fd, command_op, args, num_args,
				    &hdr, sizeof(hdr));
	if (rc < 0)
		return rc;
	// sync_handle is the completion sync_file fd the driver returned.
	*out_handle = static_cast<uint32_t>(hdr.sync_handle);
	return 0;
}

int do_syncobj_wait(int device_fd, uint32_t handle, uint64_t timeout_ns)
{
	int32_t status = 0;
	// wait_complete returns 0 once the fence is signalled -- even when
	// it carries an error (poll wakes on signalling; the error is only
	// visible via fence_status). Fault subtests rely on that: "signalled
	// with error" is still signalled, so the wait itself succeeds.
	return amdrnpu_syncobj_wait_complete(device_fd, handle,
					     static_cast<int64_t>(timeout_ns),
					     &status);
}

int do_syncobj_destroy(int device_fd, uint32_t handle)
{
	(void)device_fd;
	if (::close(static_cast<int>(handle)) < 0)
		return -errno;
	return 0;
}

int do_buf_alloc(int device_fd, uint32_t mtype, uint64_t size, uint32_t *out_handle)
{
	return amdrnpu_buf_alloc(device_fd, mtype, size, out_handle);
}

int do_gem_close(int device_fd, uint32_t handle)
{
	return amdrnpu_buf_close(device_fd, handle);
}

int do_buf_mmap(int device_fd, uint32_t handle, uint64_t size, void **out_map)
{
	// offset=0, length=0 -> map the whole buffer (lib fills length from
	// the registry). buf_size is reserved.
	return amdrnpu_buf_mmap(device_fd, handle, size, 0, 0, out_map);
}

void do_buf_munmap(void *addr, uint64_t length)
{
	amdrnpu_buf_munmap(addr, length);
}

int do_buf_sync(int device_fd, uint32_t handle, uint32_t direction,
		uint64_t offset, uint64_t length)
{
	return amdrnpu_buf_sync(device_fd, handle, direction, offset, length);
}

int do_syncobj_to_fence_fd(int device_fd, uint32_t handle, int *out_sf_fd)
{
	return amdrnpu_syncobj_to_fence_fd(device_fd, handle, out_sf_fd);
}

int do_syncobj_fence_status(int device_fd, uint32_t handle, int32_t *out_status)
{
	return amdrnpu_syncobj_fence_status(device_fd, handle, out_status);
}

int do_syncobj_busy_poll(int device_fd, uint32_t handle,
			 uint64_t max_wall_ns, uint64_t interval_ns,
			 int32_t *out_status)
{
	return amdrnpu_syncobj_wait_busy_poll(device_fd, handle,
					      static_cast<int64_t>(max_wall_ns),
					      interval_ns, out_status);
}

// EL0 DC CIVAC over a VA range. Standard arm64 idiom amdgpu/habanalabs/
// xdna use for "userspace cache management on close-before-sync paths".
// SCTLR_EL1.UCI=1 is the default on every Linux arm64 kernel.
#if defined(__aarch64__)
void uspace_dcache_invalidate(const void *vaddr, size_t length)
{
	uint64_t ctr;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	size_t cline_sz = static_cast<size_t>(4) << ((ctr >> 16) & 0xfu);

	uintptr_t start = reinterpret_cast<uintptr_t>(vaddr);
	uintptr_t end = start + length;
	start &= ~(cline_sz - 1);
	end = (end + cline_sz - 1) & ~(cline_sz - 1);

	for (uintptr_t p = start; p < end; p += cline_sz)
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}
#else
void uspace_dcache_invalidate(const void *, size_t) {}
#endif

}  // namespace amdrnpu_test
