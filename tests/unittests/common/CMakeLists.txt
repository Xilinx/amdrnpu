# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.

# utest_common: small shared static lib with generic test helpers
# (KB/MB/GB macros, page size, deterministic buffer fill/verify, exec
# path resolver). Mirrors testcases-v2/unittests/common/.
add_library(utest_common STATIC
	utils.cpp
	utils.h
)

set_target_properties(utest_common PROPERTIES
	CXX_STANDARD 17
	CXX_STANDARD_REQUIRED ON
	POSITION_INDEPENDENT_CODE ON
)

target_include_directories(utest_common
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(utest_common PRIVATE
	-Wall -Wextra -Wno-unused-parameter
)
