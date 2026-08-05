// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.

#include "common.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>

namespace amdrnpu_test {

int drm_get_name(int fd, char *out, size_t out_sz)
{
	struct drm_version v {};
	char name[64] = {};
	v.name = name;
	v.name_len = sizeof(name) - 1;
	if (::ioctl(fd, DRM_IOCTL_VERSION, &v) < 0)
		return -errno;
	std::snprintf(out, out_sz, "%.*s", static_cast<int>(v.name_len), name);
	return 0;
}

int open_amdrnpu_card(char *out_path, size_t out_sz)
{
	DIR *dir = ::opendir("/dev/dri");
	if (!dir)
		return -1;

	int found = -1;
	struct dirent *e;
	while ((e = ::readdir(dir)) != nullptr) {
		if (std::strncmp(e->d_name, "card", 4) != 0)
			continue;
		if (!std::isdigit(static_cast<unsigned char>(e->d_name[4])))
			continue;

		char path[PATH_MAX];
		std::snprintf(path, sizeof(path), "/dev/dri/%s", e->d_name);

		int fd = ::open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;

		char name[64] = {};
		if (drm_get_name(fd, name, sizeof(name)) == 0 &&
		    std::strcmp(name, AMDRNPU_DRIVER_NAME) == 0) {
			if (out_path && out_sz)
				std::snprintf(out_path, out_sz, "%s", path);
			found = fd;
			break;
		}
		::close(fd);
	}
	::closedir(dir);
	return found;
}

uint64_t now_ns()
{
	struct timespec ts {};
	::clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull +
	       static_cast<uint64_t>(ts.tv_nsec);
}

int do_cmd_submit(int fd, uint32_t command_op,
		  const struct amdrnpu_arg *args, uint32_t num_args,
		  uint32_t *out_handle)
{
	size_t bytes = sizeof(struct amdrnpu_cmd_submit) +
		       static_cast<size_t>(num_args) * sizeof(struct amdrnpu_arg);
	auto *hdr = static_cast<struct amdrnpu_cmd_submit *>(std::calloc(1, bytes));
	if (!hdr)
		return -ENOMEM;

	hdr->command_op = command_op;
	hdr->num_args   = num_args;
	if (num_args && args)
		std::memcpy(hdr->args, args, num_args * sizeof(*args));

	int rc = 0;
	if (::ioctl(fd, AMDRNPU_CMD_SUBMIT, hdr) < 0)
		rc = -errno;
	else
		*out_handle = static_cast<uint32_t>(hdr->sync_handle);

	std::free(hdr);
	return rc;
}

int do_syncobj_wait(int fd, uint32_t handle, uint64_t timeout_ns)
{
	struct drm_syncobj_wait w {};
	uint64_t handles64 = handle;
	w.handles        = reinterpret_cast<uint64_t>(&handles64);
	w.timeout_nsec   = static_cast<int64_t>(now_ns() + timeout_ns);
	w.count_handles  = 1;
	w.flags          = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
	w.first_signaled = 0;
	if (::ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &w) < 0)
		return -errno;
	return 0;
}

int do_syncobj_destroy(int fd, uint32_t handle)
{
	struct drm_syncobj_destroy d {};
	d.handle = handle;
	if (::ioctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &d) < 0)
		return -errno;
	return 0;
}

int do_buf_alloc(int fd, uint32_t mtype, uint64_t size, uint32_t *out_handle)
{
	struct amdrnpu_buf_alloc a {};
	a.memory_type = mtype;
	a.size        = size;
	if (::ioctl(fd, AMDRNPU_BUF_ALLOC, &a) < 0)
		return -errno;
	*out_handle = a.drm_handle;
	return 0;
}

int do_gem_close(int fd, uint32_t handle)
{
	struct drm_gem_close c {};
	c.handle = handle;
	if (::ioctl(fd, DRM_IOCTL_GEM_CLOSE, &c) < 0)
		return -errno;
	return 0;
}

int do_map_dumb(int fd, uint32_t handle, uint64_t *out_offset)
{
	struct drm_mode_map_dumb m {};
	m.handle = handle;
	if (::ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m) < 0)
		return -errno;
	*out_offset = m.offset;
	return 0;
}

int do_buf_sync(int fd, uint32_t handle, uint32_t direction,
		uint64_t offset, uint64_t length)
{
	struct amdrnpu_buf_sync s {};
	s.drm_handle = handle;
	s.direction  = direction;
	s.offset     = offset;
	s.length     = length;
	if (::ioctl(fd, AMDRNPU_BUF_SYNC, &s) < 0)
		return -errno;
	return 0;
}

int do_syncobj_to_fence_fd(int fd, uint32_t handle, int *out_sf_fd)
{
	struct drm_syncobj_handle h {};
	h.handle = handle;
	h.flags  = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
	if (::ioctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &h) < 0)
		return -errno;
	*out_sf_fd = static_cast<int>(h.fd);
	return 0;
}

int do_syncobj_fence_status(int fd, uint32_t handle, int32_t *out_status)
{
	int sf = -1;
	int rc = do_syncobj_to_fence_fd(fd, handle, &sf);
	if (rc < 0)
		return rc;

	struct sync_file_info fi {};
	if (::ioctl(sf, SYNC_IOC_FILE_INFO, &fi) < 0) {
		rc = -errno;
		::close(sf);
		return rc;
	}
	::close(sf);
	*out_status = fi.status;
	return 0;
}

int do_syncobj_busy_poll(int fd, uint32_t handle,
			 uint64_t max_wall_ns, uint64_t interval_ns,
			 int32_t *out_status)
{
	uint64_t t0 = now_ns();
	for (;;) {
		int rc = do_syncobj_fence_status(fd, handle, out_status);
		if (rc < 0)
			return rc;
		if (*out_status != 0)
			return 0;
		if (now_ns() - t0 >= max_wall_ns)
			return -ETIMEDOUT;
		if (interval_ns > 0) {
			struct timespec rq {
				.tv_sec  = static_cast<time_t>(interval_ns / 1'000'000'000ull),
				.tv_nsec = static_cast<long>(interval_ns % 1'000'000'000ull),
			};
			while (::nanosleep(&rq, &rq) < 0 && errno == EINTR)
				;
		} else {
			(void)::sched_yield();
		}
	}
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
