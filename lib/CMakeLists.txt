# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)

project(rpurt_rpc
    VERSION 0.1.0
    DESCRIPTION "RPU runtime RPC user-space library"
    LANGUAGES C
)

include(GNUInstallDirs)

option(RPURT_RPC_BUILD_SHARED  "Build the shared library"     ON)
option(RPURT_RPC_BUILD_STATIC  "Build the static library"     ON)
option(RPURT_RPC_BUILD_EXAMPLES "Build example programs"      ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# The shared UAPI header (also consumed by the kernel module) lives at
# <repo>/include/uapi/rpurt-rpc/rpurt_rpc.h. Expose it via the public
# include directory of this library.
set(RPURT_RPC_UAPI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../include/uapi"
    CACHE PATH "Path to the shared UAPI headers")

# Note: the vendored firmware mailbox wire format (lib/src/aie_msg.h)
# is intentionally NOT exposed as a public include directory and is
# NOT installed - it's an implementation detail of how this lib
# speaks to the firmware, not part of the lib's public ABI.  If the
# driver ever takes over framing (xdna-driver style), the header
# moves to drivers/misc/rpurt-rpc/ along with that change.

set(RPURT_RPC_PUBLIC_HEADERS
    include/rpurt_rpc/rpurt_rpc.h
)

set(RPURT_RPC_SOURCES
    src/rpurt_rpc.c
)

# ----------------------------------------------------------------------------
# Library targets
# ----------------------------------------------------------------------------
function(_rpurt_rpc_configure_target tgt)
    target_include_directories(${tgt}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${RPURT_RPC_UAPI_DIR}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(${tgt} PROPERTIES
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
endfunction()

set(_installed_targets)

if(RPURT_RPC_BUILD_SHARED)
    add_library(rpurt_rpc SHARED ${RPURT_RPC_SOURCES})
    add_library(rpurt_rpc::rpurt_rpc ALIAS rpurt_rpc)
    set_target_properties(rpurt_rpc PROPERTIES
        VERSION   ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        OUTPUT_NAME rpurt_rpc
    )
    target_compile_definitions(rpurt_rpc PRIVATE RPURT_RPC_BUILDING_DLL)
    _rpurt_rpc_configure_target(rpurt_rpc)
    list(APPEND _installed_targets rpurt_rpc)
endif()

if(RPURT_RPC_BUILD_STATIC)
    add_library(rpurt_rpc_static STATIC ${RPURT_RPC_SOURCES})
    add_library(rpurt_rpc::rpurt_rpc_static ALIAS rpurt_rpc_static)
    set_target_properties(rpurt_rpc_static PROPERTIES
        OUTPUT_NAME rpurt_rpc
    )
    _rpurt_rpc_configure_target(rpurt_rpc_static)
    list(APPEND _installed_targets rpurt_rpc_static)
endif()

# ----------------------------------------------------------------------------
# Examples
# ----------------------------------------------------------------------------
if(RPURT_RPC_BUILD_EXAMPLES)
    add_executable(rpurt_rpc_example examples/rpurt_rpc_example.c)
    if(TARGET rpurt_rpc)
        target_link_libraries(rpurt_rpc_example PRIVATE rpurt_rpc::rpurt_rpc)
    else()
        target_link_libraries(rpurt_rpc_example PRIVATE rpurt_rpc::rpurt_rpc_static)
    endif()
endif()

# ----------------------------------------------------------------------------
# Install
# ----------------------------------------------------------------------------
install(TARGETS ${_installed_targets}
    EXPORT  rpurt_rpcTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/rpurt_rpc
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

install(DIRECTORY ${RPURT_RPC_UAPI_DIR}/rpurt-rpc
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

if(RPURT_RPC_BUILD_EXAMPLES)
    install(TARGETS rpurt_rpc_example
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

# pkg-config file
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/rpurt_rpc.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/rpurt_rpc.pc
    @ONLY
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/rpurt_rpc.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

# CMake package config
include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/rpurt_rpcConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/rpurt_rpcConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rpurt_rpc
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/rpurt_rpcConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/rpurt_rpcConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/rpurt_rpcConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rpurt_rpc
)
install(EXPORT rpurt_rpcTargets
    FILE rpurt_rpcTargets.cmake
    NAMESPACE rpurt_rpc::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rpurt_rpc
)
