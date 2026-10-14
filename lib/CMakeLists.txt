# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
# Portions of this file consist of AI-generated content.
cmake_minimum_required(VERSION 3.16)

project(amdrnpu_lib
    VERSION 0.1.0
    LANGUAGES C
)

include(GNUInstallDirs)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

option(AMDRNPU_LIB_BUILD_SHARED   "Build shared library" ON)
option(AMDRNPU_LIB_BUILD_STATIC   "Build static library" ON)
option(AMDRNPU_LIB_BUILD_EXAMPLES "Build examples" ON)
option(AMDRNPU_LIB_BUILD_RPMSG_TEST "Build hardware-free rpmsg-lite test" OFF)
option(AMDRNPU_LINK_EXAMPLES_STATIC
	"Link examples against libamdrnpu.a (no libamdrnpu.so at runtime)" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# USDT/SDT static trace markers (perf-consumable) require systemtap's
# <sys/sdt.h>. Optional: without it the AMDRNPU_TRACE* macros compile to no-ops.
include(CheckIncludeFile)
check_include_file("sys/sdt.h" AMDRNPU_HAVE_SDT)

set(AMDRNPU_UAPI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../include/uapi"
    CACHE PATH "amdrnpu UAPI")
set(AMDRNPU_LIB_SOURCES
    src/amdrnpu_lib.c
    src/amdrnpu_ini.c
    src/amdrnpu_pool.c
    src/rpmsg-lite/vring_lite.c
    src/rpmsg-lite/rpmsg_lite.c
)

set(AMDRNPU_LIB_HEADERS
    include/amdrnpu/amdrnpu_lib.h
)

function(_amdrnpu_lib_configure_target tgt)
    target_include_directories(${tgt}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/rpmsg-lite>
            $<BUILD_INTERFACE:${AMDRNPU_UAPI_DIR}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_compile_options(${tgt} PRIVATE -Wall -Wextra)
    target_link_libraries(${tgt} PRIVATE Threads::Threads)
    if(AMDRNPU_HAVE_SDT)
        target_compile_definitions(${tgt} PRIVATE AMDRNPU_HAVE_SDT)
    endif()
endfunction()

set(_installed_targets)

if(AMDRNPU_LIB_BUILD_SHARED)
    add_library(amdrnpu SHARED ${AMDRNPU_LIB_SOURCES})
    add_library(amdrnpu::amdrnpu ALIAS amdrnpu)
    _amdrnpu_lib_configure_target(amdrnpu)
    set_target_properties(amdrnpu PROPERTIES
        VERSION   ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        OUTPUT_NAME amdrnpu
    )
    list(APPEND _installed_targets amdrnpu)
endif()

if(AMDRNPU_LIB_BUILD_STATIC)
    add_library(amdrnpu_static STATIC ${AMDRNPU_LIB_SOURCES})
    add_library(amdrnpu::amdrnpu_static ALIAS amdrnpu_static)
    _amdrnpu_lib_configure_target(amdrnpu_static)
    set_target_properties(amdrnpu_static PROPERTIES OUTPUT_NAME amdrnpu)
    list(APPEND _installed_targets amdrnpu_static)
endif()

if(AMDRNPU_LIB_BUILD_EXAMPLES AND _installed_targets)
	# amdrnpu_test:          fw_echo round-trip CLI.
	# amdrnpu_inout_example: COPY_IN_OUT end-to-end through the
	#                             RPURuntime slave (see
	#                             lib/examples/README-inout-example.md).
	#                             Restored after the fac10e9 reference
	#                             merge dropped it from this list;
	#                             build/build.sh expects this binary
	#                             at bin/.
	# amdrnpu_xrt_test_example: APU_XRT_TEST ('XRTT') — full XRT
	#                                end-to-end with ELF + kernel arg BOs.
	# amdrnpu_echo_latency_example: Link A echo (AMDRNPU_FW_ECHO_OP)
	#                                round-trip latency benchmark.
	set(_tess_examples
		amdrnpu_test
		amdrnpu_inout_example
		amdrnpu_xrt_test_example
		amdrnpu_echo_latency_example)
	if(AMDRNPU_LINK_EXAMPLES_STATIC)
		if(NOT TARGET amdrnpu_static)
			message(FATAL_ERROR
				"AMDRNPU_LINK_EXAMPLES_STATIC requires AMDRNPU_LIB_BUILD_STATIC=ON")
		endif()
		set(_example_tess_lib amdrnpu::amdrnpu_static)
	elseif(TARGET amdrnpu)
		set(_example_tess_lib amdrnpu::amdrnpu)
	else()
		set(_example_tess_lib amdrnpu::amdrnpu_static)
	endif()
	foreach(_exe IN LISTS _tess_examples)
		add_executable(${_exe} examples/${_exe}.c)
		target_link_libraries(${_exe} PRIVATE ${_example_tess_lib})
		if(NOT AMDRNPU_LINK_EXAMPLES_STATIC AND TARGET amdrnpu)
			target_link_options(${_exe} PRIVATE "-Wl,-rpath,\$ORIGIN/../amdrnpu-lib")
		endif()
	endforeach()
	# Echo-latency benchmark uses sqrt() for stddev -> needs libm.
	if(TARGET amdrnpu_echo_latency_example)
		target_link_libraries(amdrnpu_echo_latency_example PRIVATE m)
	endif()
endif()

if(AMDRNPU_LIB_BUILD_RPMSG_TEST)
    enable_testing()
    add_executable(rpmsg_lite_test
        src/rpmsg-lite/rpmsg_lite_test.c
        src/rpmsg-lite/vring_lite.c
        src/rpmsg-lite/rpmsg_lite.c
    )
    target_compile_options(rpmsg_lite_test PRIVATE -Wall -Wextra)
    add_test(NAME rpmsg_lite COMMAND rpmsg_lite_test)

    # Pool allocator + INI reader unit tests (no device access).
    add_executable(amdrnpu_pool_test
        src/amdrnpu_pool_test.c
        src/amdrnpu_pool.c
        src/amdrnpu_ini.c
    )
    target_include_directories(amdrnpu_pool_test PRIVATE src)
    target_compile_options(amdrnpu_pool_test PRIVATE -Wall -Wextra)
    add_test(NAME amdrnpu_pool COMMAND amdrnpu_pool_test)
endif()

install(TARGETS ${_installed_targets}
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/amdrnpu
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

install(FILES src/rpmsg-lite/vring_lite.h src/rpmsg-lite/rpmsg_lite.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/amdrnpu
)

install(DIRECTORY ${AMDRNPU_UAPI_DIR}/amdrnpu
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

# Sample configuration (installed to the datadir; copy to /etc/amdrnpu.ini and
# edit for the target, or point $AMDRNPU_INI at it).
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../etc/amdrnpu.ini
    DESTINATION ${CMAKE_INSTALL_DATADIR}/amdrnpu
)
