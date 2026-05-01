SUMMARY = "RPU runtime RPC user-space library"
DESCRIPTION = "Thin user-space wrapper around the rpurt-rpc kernel \
driver's ioctl ABI. Lives under lib/ in the rpu-rt-rpc repository and \
provides a small C API plus the rpurt_rpc_test CLI."
HOMEPAGE = "https://github.com/Xilinx/rpu-rt-rpc"
SECTION = "libs"

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://../LICENSE;md5=02828719ecd1a9e8adb77ad57e43ba0f"

inherit cmake externalsrc

# Build directly from the in-tree source. RPURT_RPC_SRC is defined in
# conf/layer.conf and points at the repository root.
EXTERNALSRC = "${RPURT_RPC_SRC}/lib"

# Use a dedicated build directory so we don't pollute the source tree.
EXTERNALSRC_BUILD = "${WORKDIR}/build"

S = "${EXTERNALSRC}"
B = "${EXTERNALSRC_BUILD}"

# Pin the UAPI header location so CMake does not have to guess when the
# library is built from a relocated source tree.
EXTRA_OECMAKE += " \
    -DRPURT_RPC_UAPI_DIR=${RPURT_RPC_SRC}/include/uapi \
    -DRPURT_RPC_BUILD_EXAMPLES=ON \
"

# Package the test CLIs separately to keep the runtime library lean.
PACKAGES =+ "${PN}-examples"
FILES:${PN}-examples = " \
    ${bindir}/rpurt_rpc_test \
"

# Headers and pkg-config metadata go to -dev as usual.
FILES:${PN}-dev += "${includedir}/rpurt-rpc/*"

RDEPENDS:${PN}-examples = "${PN}"

# The library is useless without the kernel driver at runtime; pull it
# in by default. Comment out if you need to ship the lib without the
# module (e.g. for SDK-only consumers).
RRECOMMENDS:${PN} = "kernel-module-rpurt-rpc"
