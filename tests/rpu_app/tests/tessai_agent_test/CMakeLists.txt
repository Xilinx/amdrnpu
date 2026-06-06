# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
#
# Link A acceptance target. Builds a minimal RPURuntime ELF whose only
# job is to come up, run apu_rpmsg_platform.c's SYS_INIT, register the
# "tessai-agent" rpmsg endpoint, and then idle. The APU side
# (tessai-agent kernel driver + tessai_agent_unittest binary) drives
# all traffic; the RPU just responds.
#
# CONFIG_TESSAI_APU_RPMSG_SLAVE=y in prj.conf is what actually turns
# on the link A transport + endpoint compilation in
# src/driver/CMakeLists.txt.

cmake_minimum_required(VERSION 3.20.0)

set(COMMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../common)

# Pull the driver in as a Zephyr module so apu_rpmsg_platform.c
# and tessai_agent_endpoint.c are eligible for compilation when
# CONFIG_TESSAI_APU_RPMSG_SLAVE=y (set in this target's prj.conf).
set(TESSAI_DRIVER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../src/driver/)
list(APPEND ZEPHYR_EXTRA_MODULES ${TESSAI_DRIVER_DIR})

# Layer: common/prj.conf base, then this target's prj.conf overlay
# (which flips TESSAI_APU_RPMSG_SLAVE on).
set(CONF_FILE     ${COMMON_DIR}/prj.conf)
set(OVERLAY_CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/prj.conf)

# Devicetree overlay. Zephyr's auto-pickup keys on
# boards/<BOARD>.overlay; we use a board-independent name so plug it
# in explicitly here.
set(DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/boards/rpu.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tessai_agent_test LANGUAGES C)

# Place the zephyr/lib/open-amp/resource_table.c output's
# .resource_table section into the RSC_TABLE memory-region
# (apu_rsctbl @ 0x0A000000, see reserved.dtsi Section D).
#
# Zephyr 3.7 on cortex-r52 uses the preprocessor-based linker script
# pipeline, NOT the CMake linker generator. The idiomatic path is
# `zephyr_linker_sources(SECTIONS ...)` which appends a .ld fragment
# into the SECTIONS slot at the end of the generated linker.cmd.
# Without this, ld orphan-places .resource_table somewhere in RAM and
# the RSC_TABLE region stays empty.
zephyr_linker_sources(SECTIONS linker/resource_table.ld)

target_sources(app PRIVATE src/main.c)
