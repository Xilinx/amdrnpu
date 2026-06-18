# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)

project(tessai_agent_lib
    VERSION 0.1.0
    LANGUAGES C
)

include(GNUInstallDirs)

option(TESSAI_AGENT_LIB_BUILD_SHARED   "Build shared library" ON)
option(TESSAI_AGENT_LIB_BUILD_STATIC   "Build static library" ON)
option(TESSAI_AGENT_LIB_BUILD_EXAMPLES "Build examples" ON)
option(TESSAI_AGENT_LINK_EXAMPLES_STATIC
	"Link examples against libtessai_agent.a (no libtessai_agent.so at runtime)" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM REQUIRED libdrm)

set(TESSAI_AGENT_UAPI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../include/uapi"
    CACHE PATH "tessai-agent UAPI")
set(TESSAI_AGENT_LIB_SOURCES
    src/tessai_agent_lib.c
)

set(TESSAI_AGENT_LIB_HEADERS
    include/tessai_agent/tessai_agent_lib.h
)

function(_tessai_agent_lib_configure_target tgt)
    target_include_directories(${tgt}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${TESSAI_AGENT_UAPI_DIR}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
        PRIVATE
            ${LIBDRM_INCLUDE_DIRS}
    )
    target_link_libraries(${tgt} PRIVATE ${LIBDRM_LIBRARIES})
    target_compile_options(${tgt} PRIVATE ${LIBDRM_CFLAGS_OTHER} -Wall -Wextra)
endfunction()

set(_installed_targets)

if(TESSAI_AGENT_LIB_BUILD_SHARED)
    add_library(tessai_agent SHARED ${TESSAI_AGENT_LIB_SOURCES})
    add_library(tessai_agent::tessai_agent ALIAS tessai_agent)
    _tessai_agent_lib_configure_target(tessai_agent)
    set_target_properties(tessai_agent PROPERTIES
        VERSION   ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        OUTPUT_NAME tessai_agent
    )
    list(APPEND _installed_targets tessai_agent)
endif()

if(TESSAI_AGENT_LIB_BUILD_STATIC)
    add_library(tessai_agent_static STATIC ${TESSAI_AGENT_LIB_SOURCES})
    add_library(tessai_agent::tessai_agent_static ALIAS tessai_agent_static)
    _tessai_agent_lib_configure_target(tessai_agent_static)
    set_target_properties(tessai_agent_static PROPERTIES OUTPUT_NAME tessai_agent)
    list(APPEND _installed_targets tessai_agent_static)
endif()

if(TESSAI_AGENT_LIB_BUILD_EXAMPLES AND _installed_targets)
	# tessai_agent_test:          fw_echo round-trip CLI.
	# tessai_agent_inout_example: COPY_IN_OUT end-to-end through the
	#                             RPURuntime slave (see
	#                             lib/examples/README-inout-example.md).
	#                             Restored after the fac10e9 reference
	#                             merge dropped it from this list;
	#                             build/build.sh expects this binary
	#                             at bin/.
	set(_tess_examples
		tessai_agent_test
		tessai_agent_inout_example)
	if(TESSAI_AGENT_LINK_EXAMPLES_STATIC)
		if(NOT TARGET tessai_agent_static)
			message(FATAL_ERROR
				"TESSAI_AGENT_LINK_EXAMPLES_STATIC requires TESSAI_AGENT_LIB_BUILD_STATIC=ON")
		endif()
		set(_example_tess_lib tessai_agent::tessai_agent_static)
	elseif(TARGET tessai_agent)
		set(_example_tess_lib tessai_agent::tessai_agent)
	else()
		set(_example_tess_lib tessai_agent::tessai_agent_static)
	endif()
	foreach(_exe IN LISTS _tess_examples)
		add_executable(${_exe} examples/${_exe}.c)
		target_link_libraries(${_exe} PRIVATE ${_example_tess_lib})
		target_include_directories(${_exe} PRIVATE ${LIBDRM_INCLUDE_DIRS})
		target_link_libraries(${_exe} PRIVATE ${LIBDRM_LIBRARIES})
		target_compile_options(${_exe} PRIVATE ${LIBDRM_CFLAGS_OTHER})
		if(NOT TESSAI_AGENT_LINK_EXAMPLES_STATIC AND TARGET tessai_agent)
			target_link_options(${_exe} PRIVATE "-Wl,-rpath,\$ORIGIN/../tessai-lib")
		endif()
	endforeach()
endif()

install(TARGETS ${_installed_targets}
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/tessai_agent
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

install(DIRECTORY ${TESSAI_AGENT_UAPI_DIR}/tessai-agent
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)
