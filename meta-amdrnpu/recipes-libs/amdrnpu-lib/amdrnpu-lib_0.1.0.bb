SUMMARY = "AMD amdrnpu userspace runtime library"
DESCRIPTION = "Thin userspace wrapper around the amdrnpu DRM driver: \
AMDRNPU_BUF_ALLOC and AMDRNPU_CMD_SUBMIT plus drm_syncobj completion \
helpers. Lives under lib/ in the amdrnpu repository and \
ships the amdrnpu_test CLI used to exercise every UAPI op."
HOMEPAGE = "https://gitenterprise.xilinx.com/PhysicalAI/tessai-agent"
SECTION = "libs"

LICENSE = "MIT"
# Checksum the SPDX header line at the top of the library implementation.
# Update this md5 if you change the SPDX line in amdrnpu_lib.c.
LIC_FILES_CHKSUM = "file://src/amdrnpu_lib.c;beginline=1;endline=1;md5=234d7d4edd08962c0144e4604050e0b6"

inherit cmake externalsrc pkgconfig

# CMake project lives in lib/ at the top of the amdrnpu repository
# (the repo IS amdrnpu now, so there is no extra nesting level under
# lib/ as there was historically).
EXTERNALSRC = "${AMDRNPU_SRC}/lib"

# Use a dedicated build directory so we don't pollute the source tree.
EXTERNALSRC_BUILD = "${WORKDIR}/build"

S = "${EXTERNALSRC}"
B = "${EXTERNALSRC_BUILD}"

# amdrnpu_lib.c calls into libdrm for syncobj / sync_file ioctls and
# for DRM_IOCTL_GEM_CLOSE struct definitions.
DEPENDS = "libdrm"

# CMake exposes the UAPI dir explicitly so the lib does not have to
# guess when built from a relocated source tree.
EXTRA_OECMAKE += " \
    -DAMDRNPU_UAPI_DIR=${AMDRNPU_SRC}/include/uapi \
    -DAMDRNPU_LIB_BUILD_SHARED=ON \
    -DAMDRNPU_LIB_BUILD_STATIC=ON \
    -DAMDRNPU_LIB_BUILD_EXAMPLES=ON \
"

# Package the test CLI separately so the runtime library stays lean.
PACKAGES =+ "${PN}-examples"
FILES:${PN}-examples = " \
    ${bindir}/amdrnpu_test \
"

# Headers (both lib API and the UAPI mirrored at install time) go to -dev.
FILES:${PN}-dev += " \
    ${includedir}/amdrnpu/* \
"

RDEPENDS:${PN}-examples = "${PN}"

# The library is useless without the kernel driver at runtime; pull it
# in by default. Comment out if you need to ship the lib without the
# module (e.g. for SDK-only consumers).
RRECOMMENDS:${PN} = "kernel-module-amdrnpu"
