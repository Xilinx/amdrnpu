#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
# Portions of this file consist of AI-generated content.
#
# amdrnpu unified build orchestrator.
#
# Builds all deliverables, out-of-source, into build/<TYPE>/:
#   - drivers/rpmsg/amdrnpu/       -> amdrnpu.ko        (kernel module)
#   - lib/                        -> libamdrnpu.{so*,a}
#   - lib/examples/               -> amdrnpu_test      (CLI)
#                                    amdrnpu_inout_example (CLI;
#                                    COPY_IN_OUT smoke test)
#                                    amdrnpu_xrt_test_example (CLI;
#                                    APU_XRT_TEST full XRT end-to-end)
#                                    amdrnpu_echo_latency_example (CLI;
#                                    FW_ECHO round-trip latency)
#   - tests/unittests/            -> amdrnpu_unittest  (gtest-based
#                                    acceptance suite; skip the tests/
#                                    phase entirely with -no-tests; the
#                                    gtest binary itself is opt-in via
#                                    -unit_tests so a plain lib/kmod
#                                    build doesn't drag in a C++17
#                                    toolchain unless you ask for it.)
#
# Two modes:
#
#   Native (no flag):
#     Builds against the running kernel's headers. Output: build/Release/.
#     Requires linux-headers-$(uname -r). Produces host-arch artifacts for
#     quick compile checks and the gtest suite; not for the aarch64 board.
#
#   -sdk PATH:
#     aarch64 cross-build using an extracted Yocto-generated SDK at PATH
#     (the directory containing environment-setup-*). The SDK supplies the
#     cross toolchain, the target sysroot, and the kernel dev sources
#     (usr/src/kernel) used to build the .ko. Output: build/Release-aarch64/.
#     This is the build that targets APU Linux on the board.
#
#     Generate such an SDK from your Yocto build with:
#       bitbake <your-image> -c populate_sdk
#     then run the resulting *.sh installer to extract it, and point -sdk
#     at the install directory.
#
# The userspace library has no external dependencies (no libdrm): buffers
# are plain dma-buf objects and completion is a sync_file fd, so the lib
# talks to the char device and the dma-buf / dma-heap / sync_file
# interfaces directly. Nothing beyond the C library and the kernel UAPI
# headers is needed to build or link it.
#
# Output layout (per build):
#   build/<TYPE>/driver/    final .ko
#   build/<TYPE>/lib/       libamdrnpu.{so*,a}
#   build/<TYPE>/bin/       amdrnpu_test
#                           amdrnpu_inout_example
#                           amdrnpu_xrt_test_example
#                           amdrnpu_echo_latency_example
#                           amdrnpu_unittest         (only with -unit_tests)
#   build/<TYPE>/obj/       kbuild mirror: directories are real, source
#                           files are symlinks back into the repo, kbuild
#                           outputs (.o, .ko, .mod*, .cmd) are real files
#                           in the mirror. The repo source tree
#                           (drivers/rpmsg/amdrnpu/) is never written to.
#   build/<TYPE>/cmake-lib/ cmake out-of-source bookkeeping for lib/
#   build/<TYPE>/*.log      build / cmake logs

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$SCRIPT_DIR/.." && pwd)

# ---------- defaults ----------
SDK=""               # -sdk PATH (extracted Yocto SDK); empty => native
CLEAN=0
JOBS=$(nproc)
KDIR=""
NATIVE_KDIR_DEFAULT="/lib/modules/$(uname -r)/build"
BUILD_TESTS=1        # build tests/ tree (-no-tests to skip)
BUILD_UNITTESTS=0    # gtest amdrnpu_unittest (-unit_tests to opt in)

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
log()  { printf "${GREEN}[build]${NC} %s\n" "$*"; }
warn() { printf "${YELLOW}[build]${NC} %s\n" "$*" >&2; }
die()  { printf "${RED}[build] %s${NC}\n" "$*" >&2; exit 1; }

usage() {
    cat <<EOF
Usage: build/build.sh [OPTIONS]

  Native: build/build.sh
    Builds kernel module + userspace lib + example CLIs against the
    running kernel's headers. Host-arch artifacts in build/Release/.
    Requires linux-headers-\$(uname -r).

  Cross (Yocto SDK): build/build.sh -sdk /path/to/sdk
    aarch64 cross-build using an extracted Yocto-generated SDK (the
    directory containing environment-setup-*). The SDK provides the
    cross toolchain, target sysroot, and kernel dev sources. Artifacts
    in build/Release-aarch64/. This is the build for APU Linux on the
    board.

Options:
  -h, -help            Show this help.
  -clean               rm -rf build/Release/ build/Release-aarch64/ and exit.
  -j N                 Parallel jobs (default: nproc = $JOBS).
  -sdk PATH            Extracted Yocto SDK directory (contains
                       environment-setup-*). Enables the aarch64 cross
                       build. KDIR defaults to the SDK's kernel dev
                       sources (sysroots/<target>/usr/src/kernel).
  -kdir PATH           Override the kernel source+build tree.
                       Default native: $NATIVE_KDIR_DEFAULT
                       Default -sdk:   <sdk-target-sysroot>/usr/src/kernel
  -no-tests            Skip the tests/ target entirely (the gtest
                       amdrnpu_unittest acceptance suite). On by default;
                       this flag is mostly for fast iteration when only
                       the lib or kmod changed.
  -unit_tests          Also build tests/unittests/ (gtest-based
                       amdrnpu_unittest). OFF by default because it needs
                       a C++17 cross toolchain. The Yocto SDK ships g++,
                       so this works under -sdk.

Outputs (per build):
  build/<TYPE>/driver/amdrnpu.ko
  build/<TYPE>/lib/libamdrnpu.{so*,a}
  build/<TYPE>/bin/amdrnpu_test
  build/<TYPE>/bin/amdrnpu_inout_example
  build/<TYPE>/bin/amdrnpu_xrt_test_example
  build/<TYPE>/bin/amdrnpu_echo_latency_example
  build/<TYPE>/bin/amdrnpu_unittest (only with -unit_tests; needs C++17)
EOF
}

# ---------- flag parsing ----------
while [ $# -gt 0 ]; do
    case "$1" in
        -h|-help|--help)   usage; exit 0 ;;
        -clean)            CLEAN=1; shift ;;
        -j)                JOBS="$2"; shift 2 ;;
        -sdk)              SDK="$2"; shift 2 ;;
        -kdir)             KDIR="$2"; shift 2 ;;
        -no-tests)         BUILD_TESTS=0; shift ;;
        -unit_tests|-unit-tests)
                           BUILD_UNITTESTS=1; shift ;;
        *) die "Unknown option: $1 (try -help)" ;;
    esac
done

CROSS=0
[ -n "$SDK" ] && CROSS=1

# ---------- BUILD_TYPE + clean ----------
if [ "$CROSS" -eq 1 ]; then BUILD_TYPE="Release-aarch64"; else BUILD_TYPE="Release"; fi
OUT="$REPO/build/$BUILD_TYPE"

if [ "$CLEAN" -eq 1 ]; then
    log "Cleaning $REPO/build/Release{,-aarch64}/"
    rm -rf "$REPO/build/Release" "$REPO/build/Release-aarch64"
    exit 0
fi

# ---------- per-mode setup ----------
# CC/CXX/AR/RANLIB used by the lib + tests cmake invocations in -sdk mode.
# CROSS_CXX is only consumed by build_tests() when unittests are opted in
# (-unit_tests); the kmod + lib paths are all C.
CROSS_CC=""
CROSS_CXX=""
CROSS_AR=""
CROSS_RANLIB=""
SYSROOT=""             # target sysroot (-sdk mode)
SDK_PATH_PREFIX=""     # SDK PATH (cross + host tools) for the kbuild step

if [ "$CROSS" -eq 1 ]; then
    [ -d "$SDK" ] || die "SDK directory does not exist: $SDK"

    # The SDK's environment-setup script is the source of truth for the
    # toolchain, target sysroot, and PATH. Source it in a clean subshell
    # and capture only the values we need; we deliberately do NOT export
    # its CC/CFLAGS/LDFLAGS into our own environment, because those are
    # userspace flags that would break the kernel-module build (kbuild
    # sets its own). We drive kbuild with ARCH/CROSS_COMPILE/PATH only.
    shopt -s nullglob
    _envs=( "$SDK"/environment-setup-* )
    shopt -u nullglob
    [ "${#_envs[@]}" -gt 0 ] \
        || die "no environment-setup-* found under $SDK (is this an extracted Yocto SDK?)"
    SDK_ENV="${_envs[0]}"

    # The SDK's environment-setup refuses to initialize when LD_LIBRARY_PATH
    # is set (it can break the SDK's own tools). Clear it for the sourcing
    # subshell only; our build steps drive the toolchain via explicit
    # compiler paths and PATH, so the parent environment is left untouched.
    eval "$(
        set +eu
        unset LD_LIBRARY_PATH
        # shellcheck disable=SC1090
        . "$SDK_ENV" >/dev/null 2>&1
        printf 'SDK_SYSROOT=%q\n'        "${SDKTARGETSYSROOT:-}"
        printf 'SDK_NATIVE=%q\n'         "${OECORE_NATIVE_SYSROOT:-}"
        printf 'SDK_ARCH=%q\n'           "${ARCH:-}"
        printf 'SDK_CROSS_COMPILE=%q\n'  "${CROSS_COMPILE:-}"
        printf 'SDK_PATH_PREFIX=%q\n'    "${PATH:-}"
    )"

    [ -n "${SDK_SYSROOT:-}" ] && [ -d "$SDK_SYSROOT" ] \
        || die "SDK did not export a usable SDKTARGETSYSROOT (sourced $SDK_ENV)"
    [ -n "${SDK_CROSS_COMPILE:-}" ] \
        || die "SDK did not export CROSS_COMPILE (sourced $SDK_ENV)"

    SYSROOT="$SDK_SYSROOT"

    # Kernel dev sources ship inside the target sysroot. A standard SDK
    # populated with kernel-devsrc exposes them at usr/src/kernel (a
    # symlink into lib/modules/<ver>/source).
    [ -n "$KDIR" ] || KDIR="$SDK_SYSROOT/usr/src/kernel"
    [ -f "$KDIR/Makefile" ] && [ -d "$KDIR/scripts" ] \
        || die "SDK kernel dev sources not found at $KDIR.
       The SDK must be built with kernel-devsrc (TOOLCHAIN_TARGET_TASK +=
       \"kernel-devsrc\") for out-of-tree module builds, or pass -kdir PATH."

    # Cross tool binaries live in the native sysroot under a per-target
    # subdir (e.g. usr/bin/aarch64-amd-linux/). Resolve the prefix.
    _cc="${SDK_CROSS_COMPILE}gcc"
    _ccpath=$(PATH="$SDK_PATH_PREFIX" command -v "$_cc" 2>/dev/null) \
        || die "cross compiler ${_cc} not found on the SDK PATH"
    _bindir=$(dirname "$_ccpath")
    CROSS_CC="$_bindir/${SDK_CROSS_COMPILE}gcc"
    CROSS_CXX="$_bindir/${SDK_CROSS_COMPILE}g++"
    CROSS_AR="$_bindir/${SDK_CROSS_COMPILE}ar"
    CROSS_RANLIB="$_bindir/${SDK_CROSS_COMPILE}ranlib"
    [ -x "$CROSS_CC" ] || die "cross gcc not executable: $CROSS_CC"

    export ARCH="${SDK_ARCH:-arm64}"
    export CROSS_COMPILE="$SDK_CROSS_COMPILE"
else
    [ -n "$KDIR" ] || KDIR="$NATIVE_KDIR_DEFAULT"
    [ -f "$KDIR/Makefile" ] && [ -d "$KDIR/scripts" ] \
        || die "KDIR is not a kernel build tree: $KDIR
       (install linux-headers-\$(uname -r) or pass -kdir PATH)"
fi

mkdir -p "$OUT/driver" "$OUT/lib" "$OUT/bin"
log "amdrnpu build"
if [ "$CROSS" -eq 1 ]; then
    log "  Mode:         sdk cross (aarch64)"
    log "  SDK:          $SDK"
    log "  Compiler:     $("$CROSS_CC" --version 2>&1 | head -1)"
else
    log "  Mode:         native"
fi
log "  Repo:         $REPO"
log "  Output:       $OUT"
log "  KDIR:         $KDIR"
[ -n "$SYSROOT" ] && log "  Sysroot:      $SYSROOT"
log "  Jobs:         $JOBS"
echo

# ---------- kernel module phase ----------
build_kmod() {
    log "Building amdrnpu kernel module"

    # An external-module build (make M=...) needs the kernel's host-side
    # scripts (modpost, fixdep, ...) already compiled. A freshly extracted
    # Yocto kernel-devsrc ships the .config + Module.symvers but NOT the
    # built host tools, so prepare them once (idempotent; writes into the
    # SDK's own kernel tree, never the repo).
    if [ "$CROSS" -eq 1 ] && [ ! -x "$KDIR/scripts/mod/modpost" ]; then
        log "  Preparing SDK kernel tree (one-time 'make modules_prepare')"
        PATH="$SDK_PATH_PREFIX" \
            make -C "$KDIR" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" \
                 -j "$JOBS" modules_prepare >"$OUT/kmod-prepare.log" 2>&1 \
            || { cat "$OUT/kmod-prepare.log" >&2; die "modules_prepare failed"; }
    fi

    # cp -rs: directories real, files symlinked. kbuild writes its outputs
    # (.o, .ko, .mod*, .cmd, Module.symvers, modules.order) as real files
    # next to the source symlinks in the mirror, never touching the source.
    # Both drivers/ and include/ must be mirrored, because Kbuild does
    # `-I$(src)/../../../include/uapi`.
    rm -rf "$OUT/obj"
    mkdir -p "$OUT/obj"
    cp -rs "$REPO/drivers" "$OUT/obj/"
    cp -rs "$REPO/include" "$OUT/obj/"

    local MIRROR_M="$OUT/obj/drivers/rpmsg/amdrnpu"

    # Defensive: if the source tree had stale build artifacts (e.g. from
    # a previous in-source `make -C drivers/rpmsg/amdrnpu`), they show up
    # as symlinks in the mirror. Delete them so kbuild creates fresh real
    # files in their place rather than writing through a symlink back into
    # the source.
    find "$MIRROR_M" -maxdepth 1 \
        \( -name '*.ko' -o -name '*.o' -o -name '*.mod' \
           -o -name '*.mod.*' -o -name '.*.cmd' \
           -o -name 'Module.symvers' -o -name 'modules.order' \) -delete

    local -a make_env=()
    [ "$CROSS" -eq 1 ] && make_env=( env "PATH=$SDK_PATH_PREFIX" )

    "${make_env[@]}" \
        make -C "$KDIR" M="$MIRROR_M" -j "$JOBS" modules 2>&1 \
            | tee "$OUT/build-kmod.log" \
            | grep -E '^(  CC|  LD|  MODPOST|ERROR|make.*Error|WARNING)' \
            || true

    [ -f "$MIRROR_M/amdrnpu.ko" ] \
        || die "amdrnpu.ko was not produced (see $OUT/build-kmod.log)"

    cp -f "$MIRROR_M/amdrnpu.ko" "$OUT/driver/"

    log "  -> $OUT/driver/amdrnpu.ko"
    /usr/sbin/modinfo "$OUT/driver/amdrnpu.ko" 2>/dev/null \
        | grep -E '^(filename|version|description|vermagic|alias|depends):' \
        || true
    echo
}

# ---------- userspace lib + CLI phase ----------
build_lib() {
    log "Building userspace lib + example CLIs"

    local cmake_args=(
        -S "$REPO/lib"
        -B "$OUT/cmake-lib"
        -DCMAKE_BUILD_TYPE=Release
        -DAMDRNPU_LIB_BUILD_EXAMPLES=ON
    )

    if [ "$CROSS" -eq 1 ]; then
        cmake_args+=(
            -DCMAKE_SYSTEM_NAME=Linux
            -DCMAKE_SYSTEM_PROCESSOR=aarch64
            -DCMAKE_C_COMPILER="$CROSS_CC"
            -DCMAKE_AR="$CROSS_AR"
            -DCMAKE_RANLIB="$CROSS_RANLIB"
            -DCMAKE_SYSROOT="$SYSROOT"
            -DCMAKE_FIND_ROOT_PATH="$SYSROOT"
            -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER
            -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY
            -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY
            -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY
            # Link the examples against libamdrnpu.a so the staged binaries
            # run on the board without needing libamdrnpu.so on the
            # rootfs loader path.
            -DAMDRNPU_LINK_EXAMPLES_STATIC=ON
        )
    fi

    cmake "${cmake_args[@]}" >"$OUT/cmake-configure.log" 2>&1 \
        || { cat "$OUT/cmake-configure.log" >&2; die "cmake configure failed"; }
    cmake --build "$OUT/cmake-lib" -j "$JOBS" 2>&1 \
        | tee "$OUT/cmake-build.log" \
        | grep -E '^\[|error:|Error|warning:' || true

    shopt -s nullglob
    local libs=( "$OUT/cmake-lib"/libamdrnpu.so* "$OUT/cmake-lib"/libamdrnpu.a )
    shopt -u nullglob
    [ "${#libs[@]}" -gt 0 ] \
        || die "no libamdrnpu.* produced (see $OUT/cmake-build.log)"
    cp -af "${libs[@]}" "$OUT/lib/"

    # Copy every example listed in lib/CMakeLists.txt _tess_examples into
    # bin/. Kept as an explicit list (not a glob over cmake-lib/) so that
    # a renamed or deleted example fails the build loudly with the
    # "was not produced" die instead of silently dropping out of the
    # artifact set. Must stay in lockstep with _tess_examples in
    # lib/CMakeLists.txt.
    local examples=(
        amdrnpu_test
        amdrnpu_inout_example
        amdrnpu_xrt_test_example
        amdrnpu_echo_latency_example
    )
    for _exe in "${examples[@]}"; do
        [ -f "$OUT/cmake-lib/$_exe" ] \
            || die "$_exe was not produced (see $OUT/cmake-build.log)"
        cp -f "$OUT/cmake-lib/$_exe" "$OUT/bin/"
    done

    log "  -> $OUT/lib/ ($(ls -1 "$OUT/lib/" | tr '\n' ' '))"
    for _exe in "${examples[@]}"; do log "  -> $OUT/bin/$_exe"; done
    echo
}

# ---------- userspace tests phase ----------
# Build tests/unittests/amdrnpu_unittest (gtest acceptance suite). The
# parent tests/ CMake project itself only contains the gate flag
# (AMDRNPU_BUILD_UNITTESTS) and the add_subdirectory call, so without
# -unit_tests the tests/ phase is effectively a no-op configure pass kept
# solely for symmetry with -no-tests.
#
# The gtest binary depends only on the kernel UAPI headers and this repo's
# include/uapi/amdrnpu/amdrnpu.h, plus a C++17 toolchain. In -sdk mode we
# use the SDK's cross g++ and target sysroot.
build_tests() {
    [ "$BUILD_TESTS" -eq 1 ] || { log "Skipping tests/ (-no-tests)"; return 0; }
    [ -f "$REPO/tests/CMakeLists.txt" ] \
        || { warn "tests/CMakeLists.txt not present, skipping"; return 0; }

    if [ "$BUILD_UNITTESTS" -ne 1 ]; then
        log "tests/ configured for unittests-OFF (no-op without -unit_tests)"
        return 0
    fi

    log "Building gtest suite (tests/unittests/)"

    local cmake_args=(
        -S "$REPO/tests"
        -B "$OUT/cmake-tests"
        -DCMAKE_BUILD_TYPE=Release
        -DAMDRNPU_BUILD_UNITTESTS=ON
        # Auto-stage on every link so incremental rebuilds
        # (`make -C $OUT/cmake-tests/unittests`) keep $OUT/bin/ in sync
        # without needing a full ./build/build.sh re-run.
        -DAMDRNPU_STAGE_BIN_DIR="$OUT/bin"
    )

    if [ "$CROSS" -eq 1 ]; then
        cmake_args+=(
            -DCMAKE_SYSTEM_NAME=Linux
            -DCMAKE_SYSTEM_PROCESSOR=aarch64
            -DCMAKE_C_COMPILER="$CROSS_CC"
            -DCMAKE_CXX_COMPILER="$CROSS_CXX"
            -DCMAKE_AR="$CROSS_AR"
            -DCMAKE_RANLIB="$CROSS_RANLIB"
            -DCMAKE_SYSROOT="$SYSROOT"
            -DCMAKE_FIND_ROOT_PATH="$SYSROOT"
            -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER
            -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY
            -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY
            -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY
        )
        [ -x "$CROSS_CXX" ] \
            || die "-unit_tests requires a cross g++ at $CROSS_CXX"
    fi

    cmake "${cmake_args[@]}" >"$OUT/cmake-tests-configure.log" 2>&1 \
        || { cat "$OUT/cmake-tests-configure.log" >&2; die "tests cmake configure failed"; }
    cmake --build "$OUT/cmake-tests" -j "$JOBS" 2>&1 \
        | tee "$OUT/cmake-tests-build.log" \
        | grep -E '^\[|error:|Error|warning:' || true

    if [ -f "$OUT/cmake-tests/unittests/amdrnpu_unittest" ]; then
        cp -f "$OUT/cmake-tests/unittests/amdrnpu_unittest" "$OUT/bin/"
        log "  -> $OUT/bin/amdrnpu_unittest"
    else
        die "amdrnpu_unittest was not produced (see $OUT/cmake-tests-build.log). \
Did you run: git submodule update --init --recursive ?"
    fi
    echo
}

# ---------- run ----------
build_kmod
build_lib
build_tests

log "Done. Artifacts under $OUT/:"
log "  driver/amdrnpu.ko"
for f in $(ls -1 "$OUT/lib/"); do log "  lib/$f"; done
for f in $(ls -1 "$OUT/bin/"); do log "  bin/$f"; done
