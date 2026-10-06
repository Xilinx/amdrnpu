# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
# Portions of this file consist of AI-generated content.
SUMMARY = "AMD amdrnpu out-of-tree rpmsg char-device kernel module"
DESCRIPTION = "Lightweight rpmsg driver that binds to the 'amdrnpu' RPMsg \
channel and exposes a miscdevice (/dev/amdrnpu.N) with a small custom ioctl \
surface (magic 'R'): dma-buf import/destroy and command submit, with \
completion reported as a sync_file fd. Source lives under \
drivers/rpmsg/amdrnpu in the amdrnpu repository."
HOMEPAGE = "https://github.com/Xilinx/amdrnpu"
SECTION = "kernel/modules"

LICENSE = "GPL-2.0-only"
# Checksum the SPDX header at the top of the driver entry source. Update
# this md5 if you change the SPDX line in amdrnpu_drv.c.
LIC_FILES_CHKSUM = "file://amdrnpu_drv.c;beginline=1;endline=2;md5=5f1db1a4251755b003fcaedee0489dba"

inherit module externalsrc

# Build the module straight from the in-tree source. AMDRNPU_SRC is
# defined in the layer's conf/layer.conf and points at the repository
# root by default.
EXTERNALSRC = "${AMDRNPU_SRC}/drivers/rpmsg/amdrnpu"
EXTERNALSRC_BUILD = "${EXTERNALSRC}"

S = "${EXTERNALSRC}"
B = "${EXTERNALSRC_BUILD}"

# Be explicit so the recipe does not depend on environment leakage from
# the kernel class.
EXTRA_OEMAKE += "KDIR=${STAGING_KERNEL_DIR}"

RPROVIDES:${PN} += "kernel-module-amdrnpu"

# amdrnpu.ko has no device-tree binding of its own: it registers an rpmsg
# driver and only creates a /dev/amdrnpu.N node once RPU firmware advertises
# the "amdrnpu" RPMsg channel and the driver binds to it. Autoloading is
# harmless (it just waits for a channel), but is left off pending bring-up
# verification; load manually or enable below.
# KERNEL_MODULE_AUTOLOAD += "amdrnpu"
