SUMMARY = "AMD tessai-agent userspace runtime library"
DESCRIPTION = "Thin userspace wrapper around the tessai-agent DRM driver: \
TESSAI_BUF_ALLOC and TESSAI_CMD_SUBMIT plus drm_syncobj completion \
helpers. Lives under lib/ in the tessai-agent repository and \
ships the tessai_agent_test CLI used to exercise every UAPI op."
HOMEPAGE = "https://gitenterprise.xilinx.com/PhysicalAI/tessai-agent"
SECTION = "libs"

LICENSE = "MIT"
# Checksum the SPDX header line at the top of the library implementation.
# Update this md5 if you change the SPDX line in tessai_agent_lib.c.
LIC_FILES_CHKSUM = "file://src/tessai_agent_lib.c;beginline=1;endline=1;md5=234d7d4edd08962c0144e4604050e0b6"

inherit cmake externalsrc pkgconfig

# CMake project lives in lib/ at the top of the tessai-agent repository
# (the repo IS tessai-agent now, so there is no extra nesting level under
# lib/ as there was historically).
EXTERNALSRC = "${TESSAI_AGENT_SRC}/lib"

# Use a dedicated build directory so we don't pollute the source tree.
EXTERNALSRC_BUILD = "${WORKDIR}/build"

S = "${EXTERNALSRC}"
B = "${EXTERNALSRC_BUILD}"

# tessai_agent_lib.c calls into libdrm for syncobj / sync_file ioctls and
# for DRM_IOCTL_GEM_CLOSE struct definitions.
DEPENDS = "libdrm"

# CMake exposes the UAPI dir explicitly so the lib does not have to
# guess when built from a relocated source tree.
EXTRA_OECMAKE += " \
    -DTESSAI_AGENT_UAPI_DIR=${TESSAI_AGENT_SRC}/include/uapi \
    -DTESSAI_AGENT_LIB_BUILD_SHARED=ON \
    -DTESSAI_AGENT_LIB_BUILD_STATIC=ON \
    -DTESSAI_AGENT_LIB_BUILD_EXAMPLES=ON \
"

# Package the test CLI separately so the runtime library stays lean.
PACKAGES =+ "${PN}-examples"
FILES:${PN}-examples = " \
    ${bindir}/tessai_agent_test \
"

# Headers (both lib API and the UAPI mirrored at install time) go to -dev.
FILES:${PN}-dev += " \
    ${includedir}/tessai_agent/* \
    ${includedir}/tessai-agent/* \
"

RDEPENDS:${PN}-examples = "${PN}"

# The library is useless without the kernel driver at runtime; pull it
# in by default. Comment out if you need to ship the lib without the
# module (e.g. for SDK-only consumers).
RRECOMMENDS:${PN} = "kernel-module-tessai-agent"
