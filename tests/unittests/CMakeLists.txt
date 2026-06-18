# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
#
# tessai-agent gtest-based unit testsuite.
#
# Layout (mirrors testcases-v2/unittests/):
#
#   tests/unittests/
#   ├── CMakeLists.txt          # this file
#   ├── common/                 # utest_common: KB/MB macros, page size,
#   │                           # deterministic buffer fill/verify
#   ├── src/                    # tessai-specific helpers + test-*.cpp
#   │   ├── common.h            # ioctl wrappers + TessaiAgentTest fixture
#   │   ├── common.cpp
#   │   ├── test-basic.cpp      # noop/scalar submit, channel-up smoke
#   │   ├── test-device.cpp     # version probe, multi-fd open/close
#   │   ├── test-error.cpp      # NEW: negative API tests
#   │   ├── test-memory.cpp     # BO alloc/mmap/buf_sync
#   │   ├── test-cmd.cpp        # fw_echo writeback + burst
#   │   ├── test-fence.cpp      # sync_file poll, busy_poll, in-flight pin
#   │   └── test-fault.cpp      # REMI/NOTS/CANC/BADC fence-errno mapping
#   └── thirdparty/
#       └── googletest/         # vendored gtest v1.17.0 (git submodule)
#
# Standalone CMake project; can be built three ways:
#
#   1. From the parent tests/ tree via build/build.sh (default ON):
#        ./build/build.sh                          # native
#        ./build/build.sh -cross-yocto YOCTO       # cross
#      Disable with TESSAI_AGENT_BUILD_UNITTESTS=0 (env or -D).
#
#   2. Standalone via cmake -S:
#        cmake -S tests/unittests -B build/utest
#        cmake --build build/utest -j
#        ctest --test-dir build/utest -V
#
#   3. Added as add_subdirectory() from tests/CMakeLists.txt when the
#      unittests/ subtree exists (parent script does this automatically).
#
# Test binary: tessai_agent_unittest. Uses gtest_discover_tests so each
# TEST() shows up as its own ctest case.

cmake_minimum_required(VERSION 3.16)

project(tessai_agent_unittests
	VERSION 0.1.0
	LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# UAPI override -- defaults to the in-repo headers but lets a packager
# (Yocto, distro) point at an installed include dir instead.
set(TESSAI_AGENT_UAPI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../include/uapi"
	CACHE PATH "tessai-agent UAPI directory")

if(NOT EXISTS "${TESSAI_AGENT_UAPI_DIR}/tessai-agent/tessai_agent.h")
	message(FATAL_ERROR
		"TESSAI_AGENT_UAPI_DIR='${TESSAI_AGENT_UAPI_DIR}' is missing "
		"tessai-agent/tessai_agent.h.")
endif()

# ---- cross-toolchain libstdc++ header recovery ----------------------
# When CMAKE_SYSROOT points at something other than the C++ compiler's
# default sysroot (the typical -cross-vitis / -cross-yocto case where
# the user wants to link against the board rootfs's libs but not its
# headers-only-for-C view), gcc's --sysroot flag redirects ALL system
# header lookups -- including libstdc++. Two failure modes show up:
#
#   1. `#include <chrono>` fails: --sysroot drops the toolchain's
#      built-in libstdc++ search paths (they live under the default
#      sysroot, not the override sysroot). The override sysroot
#      typically does NOT ship the C++ headers for an embedded build.
#
#   2. `#include <mutex>` compiles but produces gibberish errors like
#      "cannot convert brace-enclosed initializer list to 'unsigned
#      int' in initialization of __gthread_cond_t _M_cond". This is
#      a __pthread_cond_s ABI mismatch: GCC's include-fixed/pthread.h
#      (baked at toolchain-build time) hardcodes PTHREAD_COND_INITIALIZER
#      with the OLD struct layout (7 fields, with __g_refs[2]), but
#      the override sysroot's <bits/thread-shared-types.h> defines
#      __pthread_cond_s with the NEW layout (8 fields, no __g_refs).
#      include-fixed/pthread.h has higher precedence than the
#      sysroot's pthread.h, so the initializer comes from one glibc
#      patch level and the struct comes from another.
#
# Both are fixed by prepending the override sysroot's own /usr/include
# tree via -isystem: -isystem paths are searched BEFORE the GCC
# internal include-fixed dir, so the override sysroot's pthread.h
# (which matches its own __pthread_cond_s layout) wins, and the
# override sysroot's <chrono> / <mutex> / etc. live alongside it. We
# do NOT touch link options -- libstdc++.so is found via the regular
# sysroot link search, which is exactly the layering we want.
#
# Requires the override sysroot to ship C++ headers (usr/include/c++/
# <gccver>/). For sysroots that don't, raise an error pointing at
# either swapping sysroots or skipping -unit_tests for this build.
if(CMAKE_SYSROOT)
	execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-sysroot
		OUTPUT_VARIABLE _tessai_default_sysroot
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET)
	# Resolve symlinks before comparing -- toolchains often print
	# `bin/../<target>` paths that don't text-match a user-passed
	# absolute path even when they ultimately point at the same dir.
	set(_tessai_default_sysroot_real "")
	if(_tessai_default_sysroot)
		get_filename_component(_tessai_default_sysroot_real
			"${_tessai_default_sysroot}" REALPATH)
	endif()
	get_filename_component(_tessai_active_sysroot_real
		"${CMAKE_SYSROOT}" REALPATH)
	if(_tessai_default_sysroot_real STREQUAL _tessai_active_sysroot_real)
		# No override -- compiler will find its own libstdc++ headers
		# under the default sysroot. Nothing to do.
	else()
		# Probe the override sysroot for a matching libstdc++ tree.
		# We try a few common locations (Yocto, Debian multiarch) and
		# pick the highest version we find. Per-arch subdir is named
		# after the compiler's -dumpmachine output (e.g. aarch64-
		# amd-linux, aarch64-linux-gnu).
		execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -dumpmachine
			OUTPUT_VARIABLE _tessai_gcc_triple
			OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET)
		file(GLOB _tessai_sysroot_cxx_candidates
			"${CMAKE_SYSROOT}/usr/include/c++/[0-9]*"
			"${CMAKE_SYSROOT}/usr/include/${_tessai_gcc_triple}/c++/[0-9]*")
		set(_tessai_sysroot_cxx_inc_root "")
		foreach(_c IN LISTS _tessai_sysroot_cxx_candidates)
			if(IS_DIRECTORY "${_c}"
			    AND (NOT _tessai_sysroot_cxx_inc_root
			         OR "${_c}" STRGREATER "${_tessai_sysroot_cxx_inc_root}"))
				set(_tessai_sysroot_cxx_inc_root "${_c}")
			endif()
		endforeach()
		if(_tessai_sysroot_cxx_inc_root)
			message(STATUS
				"tessai_agent_unittest: CMAKE_SYSROOT="
				"${CMAKE_SYSROOT} differs from compiler default "
				"(${_tessai_default_sysroot}); restoring libstdc++ "
				"headers from override sysroot via -isystem "
				"${_tessai_sysroot_cxx_inc_root}/ (and prepending "
				"the sysroot's /usr/include so the matching "
				"pthread.h takes precedence over GCC's stale "
				"include-fixed copy).")
			# Order matters: C++ headers first so libstdc++'s
			# <cstdlib> etc. resolve correctly via #include_next
			# (cstdlib does #include_next <stdlib.h> and that
			# search starts AFTER cstdlib's own path -- if
			# /usr/include came first, the next-search would skip
			# right past it and stdlib.h would be unfindable).
			# Then the sysroot's /usr/include, which still lands
			# BEFORE GCC's internal include-fixed/ directory and
			# therefore shadows its stale pthread.h (the
			# __pthread_cond_s ABI mismatch fix).
			add_compile_options(
				"-isystem${_tessai_sysroot_cxx_inc_root}"
				"-isystem${_tessai_sysroot_cxx_inc_root}/${_tessai_gcc_triple}"
				"-isystem${_tessai_sysroot_cxx_inc_root}/backward"
				"-isystem${CMAKE_SYSROOT}/usr/include")
		else()
			message(FATAL_ERROR
				"tessai_agent_unittest: -unit_tests + cross sysroot "
				"override (CMAKE_SYSROOT=${CMAKE_SYSROOT}) but the "
				"sysroot has no C++ headers under usr/include/c++/. "
				"Either rebuild the sysroot with libstdc++-dev "
				"installed, drop -unit_tests for this cross build, "
				"or remove -DCMAKE_SYSROOT and let the toolchain use "
				"its default sysroot (${_tessai_default_sysroot}).")
		endif()
	endif()
endif()

# ---- vendored googletest --------------------------------------------
# Bring gtest in as a subdir. INSTALL_GTEST=OFF so consumers building
# this with `cmake --install` don't accidentally end up installing
# google's headers system-wide.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
if(NOT TARGET gtest)
	if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/googletest/CMakeLists.txt")
		add_subdirectory(thirdparty/googletest
			"${CMAKE_BINARY_DIR}/googletest_build")
	else()
		message(FATAL_ERROR
			"thirdparty/googletest is missing. "
			"Run: git submodule update --init --recursive")
	endif()
endif()

# ---- utest_common helper lib ----------------------------------------
add_subdirectory(common)

# ---- the test binary ------------------------------------------------
file(GLOB TESSAI_UNITTEST_SRCS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/test-*.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/common.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/common.h"
)

add_executable(tessai_agent_unittest ${TESSAI_UNITTEST_SRCS})

target_include_directories(tessai_agent_unittest
	PRIVATE
		${TESSAI_AGENT_UAPI_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(tessai_agent_unittest PRIVATE
	-Wall -Wextra -Wno-unused-parameter
)

find_package(Threads REQUIRED)
target_link_libraries(tessai_agent_unittest
	PRIVATE
		utest_common
		gtest
		gtest_main
		Threads::Threads
)

# ---- ctest registration ---------------------------------------------
enable_testing()
include(GoogleTest)
# DISCOVERY_MODE selects WHEN gtest_discover_tests runs the test binary
# to enumerate its TEST() cases:
#
#   POST_BUILD (CMake default): right after linking, as a build step.
#     Build fails hard if the binary can't be executed -- which is
#     always the case for cross builds where the build host has no
#     binfmt_misc registration for the target ABI.
#   PRE_TEST: lazily at `ctest` invocation time. Build never tries to
#     execute the binary, so cross builds succeed and `ctest`
#     (if ever run on the build host) just lists nothing.
#
# We pick PRE_TEST whenever the build host's CPU differs from the
# target's, which is the actual condition under which the just-linked
# binary cannot be exec()d in-place. DON'T key this on
# CMAKE_CROSSCOMPILING: CMake sets that purely on a CMAKE_SYSTEM_NAME
# mismatch, so aarch64-on-x86_64 Linux (CMAKE_SYSTEM_NAME=Linux on both
# sides) evaluates to FALSE even though the build host obviously
# cannot run the binary. We also don't try to detect qemu-user
# binfmt_misc: it's not reliably present, find_program ignores PATH
# overrides, and even when the runner is installed binfmt may not be
# registered. Native dev loses build-time discovery in exchange for
# discovery-at-first-ctest, which is a one-time ~100 ms delay.
#
# DISCOVERY_TIMEOUT default 5s is fine for a native run; bump for slow
# cross sysroots that take longer to spin up the binary under qemu.
if(CMAKE_SYSTEM_PROCESSOR STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR)
	set(_tessai_gtest_discovery_mode POST_BUILD)
else()
	set(_tessai_gtest_discovery_mode PRE_TEST)
	message(STATUS
		"tessai_agent_unittest: cross-compiling for "
		"${CMAKE_SYSTEM_PROCESSOR} on "
		"${CMAKE_HOST_SYSTEM_PROCESSOR} -> using PRE_TEST gtest "
		"discovery (deferred to ctest invocation; build step "
		"never tries to exec() the target binary).")
endif()
gtest_discover_tests(tessai_agent_unittest
	DISCOVERY_MODE ${_tessai_gtest_discovery_mode}
	PROPERTIES TIMEOUT 30
	DISCOVERY_TIMEOUT 30
)

# ---- install ---------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS tessai_agent_unittest
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ---- auto-stage for build/build.sh ---------------------------------
# build/build.sh does a one-shot `cp -f cmake-tests/unittests/
# tessai_agent_unittest $OUT/bin/` at the END of build_tests(), so the
# very first build wires up $OUT/bin/tessai_agent_unittest for
# deployment to the board. But incremental rebuilds done straight
# against the cmake-tests subtree (the common "edit a .cpp,
# `make -C build/<TYPE>/cmake-tests/unittests`" dev loop) skip that
# stage step, leaving $OUT/bin/tessai_agent_unittest stale.
#
# That asymmetry caused a real bug we want to never see again: a
# rebuild compiled clean, the dev deployed the staged binary to the
# board, but the board ran the OLD staged copy and the failing test
# reproduced unchanged. To make this self-healing, we wire up a
# POST_BUILD copy_if_different driven by a cache variable that
# build.sh sets to "$OUT/bin"; native standalone CMake builds (option
# 2 in the file header, e.g. `cmake -S tests/unittests -B build/utest`)
# leave it unset and skip the stage cleanly.
# Single-line docstring is required: CMake's set(... CACHE TYPE "doc")
# treats subsequent strings as extra positional args, not as
# concatenation -- multi-string docstrings break the cache entry in a
# way that's silent until POST_BUILD interpolation produces a garbled
# path like "Auto-staging -> ;CACHE;PATH;Directory...".
set(TESSAI_AGENT_STAGE_BIN_DIR "" CACHE PATH "Directory to auto-stage tessai_agent_unittest into after each link; set by build/build.sh to $OUT/bin so board deployment always sees the freshly-built binary. Empty -> stage skipped (standalone cmake -S tests/unittests case).")
if(TESSAI_AGENT_STAGE_BIN_DIR)
	# copy_if_different keeps mtime stable when the link did NOT
	# actually change the binary (e.g. recompile of an unrelated .cpp
	# that didn't affect the link's output), which keeps make-based
	# downstream "is the binary newer than the board's copy?" checks
	# working as expected.
	add_custom_command(TARGET tessai_agent_unittest POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E make_directory
			"${TESSAI_AGENT_STAGE_BIN_DIR}"
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"$<TARGET_FILE:tessai_agent_unittest>"
			"${TESSAI_AGENT_STAGE_BIN_DIR}/"
		COMMENT "Auto-staging tessai_agent_unittest -> ${TESSAI_AGENT_STAGE_BIN_DIR}/"
		VERBATIM)
endif()
