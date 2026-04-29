SUMMARY = "RPU runtime RPC out-of-tree kernel module"
DESCRIPTION = "Misc character driver providing an ioctl-based RPC channel \
between userspace and the RPU runtime. Source lives under \
drivers/misc/rpurt-rpc in the rpu-rt-rpc repository."
HOMEPAGE = "https://github.com/Xilinx/rpu-rt-rpc"
SECTION = "kernel/modules"

LICENSE = "GPL-2.0-only"
# Checksum the SPDX header at the top of the driver source. Update this
# md5 if you change the SPDX line in rpurt_rpc.c.
LIC_FILES_CHKSUM = "file://rpurt_rpc.c;beginline=1;endline=2;md5=5f1db1a4251755b003fcaedee0489dba"

inherit module externalsrc

# Build the module straight from the in-tree source. RPURT_RPC_SRC is
# defined in the layer's conf/layer.conf and points at the repository
# root by default; override it in local.conf to point at a different
# checkout if needed.
EXTERNALSRC = "${RPURT_RPC_SRC}/drivers/misc/rpurt-rpc"
EXTERNALSRC_BUILD = "${EXTERNALSRC}"

S = "${EXTERNALSRC}"
B = "${EXTERNALSRC_BUILD}"

# The Makefile reads either KDIR or KERNEL_SRC; be explicit so the
# recipe does not depend on environment leakage from the kernel class.
EXTRA_OEMAKE += "KDIR=${STAGING_KERNEL_DIR}"

RPROVIDES:${PN} += "kernel-module-rpurt-rpc"
KERNEL_MODULE_AUTOLOAD += "rpurt_rpc"
