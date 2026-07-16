// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
//
// Generic test utilities shared across the amdrnpu gtest suites.
// Modeled on testcases-v2/unittests/common/utils.h so contributors used
// to that layout don't have to relearn anything.
//
// Anything amdrnpu-specific (DRM card discovery, ioctl wrappers, fault
// opcodes) lives in tests/unittests/src/common.h, NOT here.

#ifndef AMDRNPU_TEST_UTILS_H_
#define AMDRNPU_TEST_UTILS_H_

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#define KB(n) ((n) * 1024ULL)
#define MB(n) (KB(n) * 1024ULL)
#define GB(n) (MB(n) * 1024ULL)

namespace amdrnpu_test {

// Returns the OS page size (sysconf(_SC_PAGESIZE) on POSIX).
long get_page_size();

// Returns the absolute path of the running test executable. Useful when
// tests want to locate data files installed alongside the binary.
std::filesystem::path get_exec_path();

inline std::filesystem::path get_exec_dir()
{
	auto exec_path = get_exec_path();
	return exec_path.empty() ? std::filesystem::path{} : exec_path.parent_path();
}

// Buffer fill / verify helpers used by test-memory. Both treat the buffer
// as a stream of uint32_t and use a deterministic pattern so a failing
// verify can pinpoint the first byte that differs.
void reset_buffer(void *host_ptr, size_t size);
void fill_buffer32(void *host_ptr, size_t size);
bool verify_buffer32(const void *host_ptr, size_t size);

// Time the execution of @f and return the wall-clock duration in usec.
template <typename F>
uint64_t get_time_elapsed_usec(F &&f)
{
	auto start = std::chrono::high_resolution_clock::now();
	f();
	auto end = std::chrono::high_resolution_clock::now();
	return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}

}  // namespace amdrnpu_test

#endif  // AMDRNPU_TEST_UTILS_H_
