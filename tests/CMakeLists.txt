# SPDX-License-Identifier: MIT
#
# amdrnpu Link A test suite.
#
# Single gtest binary (amdrnpu_unittest) that exercises every
# Link A wire behavior the slave (RPURuntime) advertises. The C
# smoke binary that used to live alongside this (tests/src/
# amdrnpu_lifecycle.c) was retired in favor of the gtest suite -- each
# of its 14 subtests has a direct TEST_F equivalent under unittests/
# (the per-suite header comments call out which one), and ctest is
# how CI is shaped now. If you need a libdrm-free C smoke binary
# again, resurrect amdrnpu_lifecycle.c from git history.
#
# Coverage map:
#   - Channel-up, COMPLETE response shape, scalar args, fw_echo
#     opcode dispatch, burst sequential submits
#     -> AmdrnpuBasic.*, AmdrnpuCmd.BurstSequentialSubmit
#   - fw_echo writeback into RPU-bank carveout + sscanf verification
#     of the canonical banner
#     -> AmdrnpuCmd.FwEchoWriteback
#   - Tier A (async/poll lifetime): poll on sync_file fd, fence_status
#     query, busy-poll, in-flight pin survives GEM_CLOSE
#     -> AmdrnpuFence.*
#   - Tier B (fault-class fence-errno mapping): REMI / NOTS / CANC / BADC
#     -> AmdrnpuFault.*
#   - Partial-window BUF_SYNC round trip via fw_echo_region
#     -> AmdrnpuMemory.BufSyncPartialRoundTrip
#   - DEVICE-bank PA round trip via AMDRNPU_FW_OP_DEVICE_ADDR_PING
#     ('DAPP'): adaptive (1 GiB -> halving fallback) BO from
#     buffer@50000000000 (the 4 GiB high-DDR reusable CMA pool at
#     0x500_0000_0000, bound to bank@1 by the official Versal-net
#     phandle <0x01>), kernel-patched PA echoed back into a separate
#     small carveout BO -- proves the wire carries
#     APU-only PAs faithfully even though the RPU has no MPU window
#     covering device shmem.
#     -> AmdrnpuCmd.DeviceShmemAddrRoundTrip
#   - Negative API tests (no amdrnpu_lifecycle equivalent)
#     -> AmdrnpuError.*, AmdrnpuDevice.MultipleOpenClose
#
# The gtest harness drives the device through libamdrnpu (linked into
# amdrnpu_unittest), which talks the amdrnpu char-device UAPI
# (amdrnpu/amdrnpu.h) -- no DRM/libdrm headers. So the suite
# cross-builds against any sysroot that ships the amdrnpu UAPI header.
# The only additional requirement vs the old C smoke binary is a C++17
# toolchain (libstdc++ <chrono>, <thread>, <atomic>, <filesystem>),
# which is why build.sh keeps the unittests gated behind -unit_tests.
#
# Builds out-of-source. Build via build/build.sh -unit_tests
# (AMDRNPU_BUILD_UNITTESTS=1, default OFF in build.sh, default
# ON when cmake -S tests is invoked directly) or directly:
#
#   cmake -S tests -B build/tests-native
#   cmake --build build/tests-native -j
#   ctest --test-dir build/tests-native -V
#
# Cross-build (Yocto-style):
#
#   cmake -S tests -B build/tests-aarch64 \
#         -DCMAKE_SYSTEM_NAME=Linux \
#         -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
#         -DCMAKE_C_COMPILER=aarch64-amd-linux-gcc \
#         -DCMAKE_CXX_COMPILER=aarch64-amd-linux-g++ \
#         -DCMAKE_SYSROOT=$SYSROOT
#   cmake --build build/tests-aarch64 -j

cmake_minimum_required(VERSION 3.16)

# The C language is enabled here so cmake's stock detection
# (CMAKE_INSTALL_BINDIR etc.) works without dragging in a CXX probe
# until we actually need one. The CXX probe lives inside the
# add_subdirectory(unittests) guard below so a
# -DAMDRNPU_BUILD_UNITTESTS=OFF cross build never runs cmake's
# C++ compiler probe. The probe is what historically broke on
# rpu-fw/board_boot/rootfs/, which ships timesize-64.h but no
# timesize-32.h -- harmless for C, fatal for any libstdc++ <chrono>
# include via gtest.
project(amdrnpu_tests
	VERSION 0.2.0
	LANGUAGES C
)

# Gtest-based unittests/ tree (tests/unittests/). Default ON when
# the subdir is present, so direct `cmake -S tests` developer
# callers pick up amdrnpu_unittest automatically. build/build.sh
# keeps its own default OFF and explicitly passes
# -DAMDRNPU_BUILD_UNITTESTS=ON when -unit_tests is requested,
# so the cross-vitis flow stays unaffected by stricter C++ frontend
# requirements until explicitly opted in.
option(AMDRNPU_BUILD_UNITTESTS
	"Build the gtest-based unittests/ subtree (requires C++17 toolchain)"
	ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# amdrnpu UAPI (AMDRNPU_DRIVER_NAME, AMDRNPU_ABI_VERSION,
# struct amdrnpu_cmd_submit, etc.). Override -DAMDRNPU_UAPI_DIR=...
# when the tests are built out-of-tree against an installed UAPI.
set(AMDRNPU_UAPI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../include/uapi"
	CACHE PATH "amdrnpu UAPI directory")

if(NOT EXISTS "${AMDRNPU_UAPI_DIR}/amdrnpu/amdrnpu.h")
	message(FATAL_ERROR
		"AMDRNPU_UAPI_DIR='${AMDRNPU_UAPI_DIR}' is missing "
		"amdrnpu/amdrnpu.h. Point it at the repo's include/uapi "
		"or an installed include dir.")
endif()

# Pull in the gtest-based unittests/ subtree when present + opted in.
# enable_language(CXX) lives inside this guard so a
# -DAMDRNPU_BUILD_UNITTESTS=OFF cross build never triggers
# cmake's C++ compiler probe (see top of this file).
if(AMDRNPU_BUILD_UNITTESTS
	AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unittests/CMakeLists.txt")
	enable_language(CXX)
	enable_testing()
	add_subdirectory(unittests)
endif()
