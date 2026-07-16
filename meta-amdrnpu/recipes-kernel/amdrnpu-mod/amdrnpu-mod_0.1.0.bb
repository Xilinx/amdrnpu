SUMMARY = "AMD amdrnpu out-of-tree DRM/RPMsg kernel module"
DESCRIPTION = "Platform driver that registers a DRM render node \
(driver name 'amdrnpu') and an RPMsg endpoint per amd,amdrnpu \
device-tree node, exposing AMDRNPU_BUF_ALLOC and AMDRNPU_CMD_SUBMIT (magic \
'T') plus standard DRM syncobj/PRIME/GEM ioctls. Source lives under \
drivers/accel/tessai-agent in the amdrnpu repository."
HOMEPAGE = "https://gitenterprise.xilinx.com/PhysicalAI/tessai-agent"
SECTION = "kernel/modules"

LICENSE = "GPL-2.0-only"
# Checksum the SPDX header at the top of the driver entry source. Update
# this md5 if you change the SPDX line in amdrnpu_drv.c.
LIC_FILES_CHKSUM = "file://amdrnpu_drv.c;beginline=1;endline=2;md5=5f1db1a4251755b003fcaedee0489dba"

inherit module externalsrc

# Build the module straight from the in-tree source. AMDRNPU_SRC is
# defined in the layer's conf/layer.conf and points at the repository
# root by default.
EXTERNALSRC = "${AMDRNPU_SRC}/drivers/accel/tessai-agent"
EXTERNALSRC_BUILD = "${EXTERNALSRC}"

S = "${EXTERNALSRC}"
B = "${EXTERNALSRC_BUILD}"

# Be explicit so the recipe does not depend on environment leakage from
# the kernel class.
EXTRA_OEMAKE += "KDIR=${STAGING_KERNEL_DIR}"

RPROVIDES:${PN} += "kernel-module-amdrnpu"

# amdrnpu.ko probes only when matching device-tree nodes
# (compatible = "amd,amdrnpu") exist with valid bank carveouts and
# an amd,remoteproc phandle. Until the platform DT carries those nodes,
# autoload would just sit idle. Leave autoload off; load manually after
# DT integration is verified.
# KERNEL_MODULE_AUTOLOAD += "amdrnpu"
