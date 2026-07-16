// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.

#include "utils.h"

#include <climits>
#include <cstdint>
#include <cstring>
#include <unistd.h>

namespace amdrnpu_test {

long get_page_size()
{
	long sz = sysconf(_SC_PAGESIZE);
	return sz > 0 ? sz : 4096;
}

std::filesystem::path get_exec_path()
{
	char buf[PATH_MAX] = {};
	ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n <= 0)
		return {};
	return std::filesystem::path(std::string(buf, static_cast<size_t>(n)));
}

void reset_buffer(void *host_ptr, size_t size)
{
	std::memset(host_ptr, 0, size);
}

// 0xDEAD0000 | (index & 0xFFFF) -- index is the uint32_t index into the
// buffer. Trivially detectable in a hexdump and lets a verify failure
// report exactly which word is wrong.
void fill_buffer32(void *host_ptr, size_t size)
{
	auto *p = static_cast<uint32_t *>(host_ptr);
	size_t n = size / sizeof(uint32_t);
	for (size_t i = 0; i < n; ++i)
		p[i] = 0xDEAD0000u | static_cast<uint32_t>(i & 0xFFFFu);
}

bool verify_buffer32(const void *host_ptr, size_t size)
{
	auto *p = static_cast<const uint32_t *>(host_ptr);
	size_t n = size / sizeof(uint32_t);
	for (size_t i = 0; i < n; ++i) {
		uint32_t expect = 0xDEAD0000u | static_cast<uint32_t>(i & 0xFFFFu);
		if (p[i] != expect)
			return false;
	}
	return true;
}

}  // namespace amdrnpu_test
