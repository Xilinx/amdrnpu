#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# tessai-agent unified build orchestrator.
#
# Builds all deliverables, out-of-source, into build/<TYPE>/:
#   - drivers/accel/tessai-agent/ -> tessai_agent.ko        (kernel module)
#   - lib/                        -> libtessai_agent.{so*,a}
#   - lib/examples/               -> tessai_agent_test      (CLI)
#                                    tessai_agent_inout_example (CLI;
#                                    COPY_IN_OUT smoke test)
#   - tests/unittests/            -> tessai_agent_unittest  (gtest-based
#                                    Link A acceptance suite; skip the
#                                    tests/ phase entirely with
#                                    -no-tests; the gtest binary
#                                    itself is opt-in via -unit_tests
#                                    so the cross-vitis flow doesn't
#                                    drag in a C++17 toolchain unless
#                                    you ask for it.)
#
# Three modes:
#
#   Native (no flag):
#     Builds against the running kernel and the host's libdrm-dev.
#     Output: build/Release/.
#
#   -cross-vitis [-sysroot PATH]:
#     aarch64 cross using a Vitis-shipped GCC and a separately-prepared
#     Xilinx kernel tree (no Yocto required). The userspace lib needs
#     PATH to point at an aarch64 sysroot whose usr/lib/pkgconfig/
#     libdrm.pc exists (the bare Vitis toolchain does not ship libdrm).
#     Output: build/Release-aarch64/.
#
#   -cross-yocto PATH:
#     aarch64 cross using a full Yocto build at PATH. Toolchain,
#     KDIR/KOBJ, and target sysroot are all auto-discovered under
#     PATH/tmp/work-shared/<MACH>/ and PATH/tmp/work/<MACH>-amd-linux/.
#     Output: build/Release-aarch64/.
#
# Output layout (per build):
#   build/<TYPE>/driver/    final .ko
#   build/<TYPE>/lib/       libtessai_agent.{so*,a}
#   build/<TYPE>/bin/       tessai_agent_test
#                           tessai_agent_inout_example
#                           tessai_agent_unittest         (only with -unit_tests)
#   build/<TYPE>/obj/       kbuild mirror: directories are real, source
#                           files are symlinks back into the repo,
#                           kbuild outputs (.o, .ko, .mod*, .cmd) are
#                           real files in the mirror. The repo source
#                           tree (drivers/accel/tessai-agent/) is never
#                           written to. Same trick xdna-driver uses.
#   build/<TYPE>/cmake-lib/ cmake out-of-source bookkeeping for lib/
#   build/<TYPE>/*.log      build / cmake / strip-dirty logs

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$SCRIPT_DIR/.." && pwd)
STRIP_DIRTY_PY="$SCRIPT_DIR/strip-dirty-vermagic.py"

# ---------- defaults ----------
CROSS_VITIS=0
CROSS_YOCTO=0
CLEAN=0
JOBS=$(nproc)
KDIR=""
NATIVE_KDIR_DEFAULT="/lib/modules/$(uname -r)/build"
CROSS_VITIS_KDIR_DEFAULT="/proj/xsjhdstaff4/hlaccabu/kernel-6.12-vek385"
VITIS_TC="/proj/xbuilds/2025.1_PRIME_0522_1/installs/lin64/Vitis/2025.1/gnu/aarch64/lin/aarch64-linux"
LOCALVERSION="-g31626ef92ff1"
STRIP_DIRTY=1
SYSROOT=""           # -sysroot, used by -cross-vitis lib step
YOCTO_BUILD=""       # -cross-yocto PATH
YOCTO_MACHINE=""     # -yocto-machine override
BUILD_TESTS=1        # build tests/ tree (-no-tests to skip)
BUILD_UNITTESTS=0    # gtest tessai_agent_unittest (-unit_tests to opt in)

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
log()  { printf "${GREEN}[build]${NC} %s\n" "$*"; }
warn() { printf "${YELLOW}[build]${NC} %s\n" "$*" >&2; }
die()  { printf "${RED}[build] %s${NC}\n" "$*" >&2; exit 1; }

usage() {
    cat <<EOF
Usage: build/build.sh [OPTIONS]

  Native: build/build.sh
    Builds kernel module + userspace lib + example CLIs against the
    running kernel and the host's libdrm-dev. Artifacts in build/Release/.

  Cross (Vitis toolchain, no Yocto): build/build.sh -cross-vitis -sysroot PATH
    Vitis-shipped aarch64 GCC + manually-prepared Xilinx kernel tree.
    PATH points at an aarch64 sysroot with usr/lib/pkgconfig/libdrm.pc.
    Artifacts in build/Release-aarch64/.

  Cross (Yocto, all-in-one): build/build.sh -cross-yocto YOCTO_BUILD
    Yocto's aarch64-amd-linux toolchain + kernel tree + staged rootfs
    auto-discovered under YOCTO_BUILD/tmp/. Artifacts in
    build/Release-aarch64/.

Options:
  -h, -help            Show this help.
  -clean               rm -rf build/Release/ build/Release-aarch64/ and exit.
  -j N                 Parallel jobs (default: nproc = $JOBS).
  -kdir PATH           Kernel source+build tree.
                       Default native:     $NATIVE_KDIR_DEFAULT
                       Default cross-vitis: $CROSS_VITIS_KDIR_DEFAULT
                       Ignored with -cross-yocto (auto-discovered).
  -cross-vitis         Cross-build for aarch64 with the Vitis toolchain.
  -toolchain PATH      Vitis aarch64-linux toolchain root (with bin/
                       aarch64-linux-gnu-gcc). Only meaningful with
                       -cross-vitis.
                       Default: $VITIS_TC
  -sysroot PATH        aarch64 target sysroot for the cross-vitis lib
                       build. PATH must contain usr/lib/pkgconfig/
                       libdrm.pc. REQUIRED with -cross-vitis because the
                       Vitis toolchain does NOT ship libdrm. Ignored
                       with -cross-yocto (Yocto provides one).
  -cross-yocto PATH    Cross-build using a Yocto build tree at PATH.
                       Auto-discovers toolchain, KDIR/KOBJ, and target
                       sysroot. The Yocto build must have built at
                       least one module recipe (so a
                       recipe-sysroot-native exists) and ideally
                       edf-linux-disk-image (for the staged rootfs that
                       provides libdrm.pc).
  -yocto-machine MACH  Override Yocto MACHINE auto-detection (the
                       directory name under tmp/work-shared/). Only
                       meaningful with -cross-yocto.
  -localversion STR    Suffix appended to vermagic (CONFIG_LOCALVERSION).
                       Used to inject the booted kernel's git SHA suffix
                       when KDIR is a non-git copy. Only applied in
                       -cross-vitis mode; -cross-yocto uses Yocto's own
                       kernel-source git tree, which provides the right
                       suffix automatically.
                       Default: '$LOCALVERSION'
  -no-strip-dirty      Skip the post-build vermagic '-dirty' strip
                       (cross modes only).
  -no-tests            Skip the tests/ target entirely (the gtest
                       tessai_agent_unittest Link A acceptance
                       suite). On by default; this flag is mostly
                       for fast iteration when only the lib or kmod
                       changed.
  -unit_tests          Also build tests/unittests/ (gtest-based
                       tessai_agent_unittest). OFF by default because
                       it needs a C++17 cross toolchain + a sysroot
                       with libstdc++ + a complete bits/ tree
                       (some cross sysroots ship aarch64-only
                       timesize-64.h but no timesize-32.h, which
                       trips libstdc++'s <chrono> include path on
                       compilers that don't honor CMAKE_SYSROOT
                       correctly for the C++ frontend). When opted
                       in here, build.sh also sets CMAKE_CXX_COMPILER
                       to the matching cross g++.

Outputs (per build):
  build/<TYPE>/driver/tessai_agent.ko
  build/<TYPE>/lib/libtessai_agent.{so*,a}
  build/<TYPE>/bin/tessai_agent_test
  build/<TYPE>/bin/tessai_agent_inout_example
  build/<TYPE>/bin/tessai_agent_unittest (only with -unit_tests; needs C++17)
EOF
}

# ---------- flag parsing ----------
while [ $# -gt 0 ]; do
    case "$1" in
        -h|-help|--help)   usage; exit 0 ;;
        -clean)            CLEAN=1; shift ;;
        -j)                JOBS="$2"; shift 2 ;;
        -kdir)             KDIR="$2"; shift 2 ;;
        -cross-vitis)      CROSS_VITIS=1; shift ;;
        -cross-yocto)      CROSS_YOCTO=1; YOCTO_BUILD="$2"; shift 2 ;;
        -yocto-machine)    YOCTO_MACHINE="$2"; shift 2 ;;
        -toolchain)        VITIS_TC="$2"; shift 2 ;;
        -localversion)     LOCALVERSION="$2"; shift 2 ;;
        -no-strip-dirty)   STRIP_DIRTY=0; shift ;;
        -no-tests)         BUILD_TESTS=0; shift ;;
        -unit_tests|-unit-tests)
                           BUILD_UNITTESTS=1; shift ;;
        -sysroot)          SYSROOT="$2"; shift 2 ;;
        *) die "Unknown option: $1 (try -help)" ;;
    esac
done

[ $((CROSS_VITIS + CROSS_YOCTO)) -le 1 ] \
    || die "-cross-vitis and -cross-yocto are mutually exclusive"

CROSS=0
[ $CROSS_VITIS -eq 1 ] && CROSS=1
[ $CROSS_YOCTO -eq 1 ] && CROSS=1

# ---------- BUILD_TYPE + clean ----------
if [ "$CROSS" -eq 1 ]; then BUILD_TYPE="Release-aarch64"; else BUILD_TYPE="Release"; fi
OUT="$REPO/build/$BUILD_TYPE"

if [ "$CLEAN" -eq 1 ]; then
    log "Cleaning $REPO/build/Release{,-aarch64}/"
    rm -rf "$REPO/build/Release" "$REPO/build/Release-aarch64"
    exit 0
fi

# ---------- per-mode setup ----------
# CC/CXX/AR/RANLIB used by the lib + tests cmake invocations in cross
# modes. CROSS_CXX is only consumed by build_tests() when unittests are
# opted in (-unit_tests); the kmod + lib + lifecycle paths are all C.
CROSS_CC=""
CROSS_CXX=""
CROSS_AR=""
CROSS_RANLIB=""
KOBJ=""                # only set in cross-yocto mode (separate src/build trees)
KBUILD_EXTRA=()        # extra "VAR=val" args appended to the kbuild make line
NATIVE_SYSROOT_BIN=""  # for the cross-yocto PATH prefix (host-side build tools)

if [ "$CROSS_VITIS" -eq 1 ]; then
    [ -n "$KDIR" ] || KDIR="$CROSS_VITIS_KDIR_DEFAULT"
    CROSS_CC="$VITIS_TC/bin/aarch64-linux-gnu-gcc"
    CROSS_CXX="$VITIS_TC/bin/aarch64-linux-gnu-g++"
    CROSS_AR="$VITIS_TC/bin/aarch64-linux-gnu-ar"
    CROSS_RANLIB="$VITIS_TC/bin/aarch64-linux-gnu-ranlib"

    [ -x "$CROSS_CC" ] \
        || die "cross-compiler not found at $CROSS_CC (pass -toolchain PATH)"
    [ -f "$KDIR/Makefile" ] && [ -d "$KDIR/scripts" ] \
        || die "KDIR is not a kernel build tree: $KDIR (pass -kdir PATH)"
    [ -n "$SYSROOT" ] \
        || die "-cross-vitis requires -sysroot PATH for the userspace lib
       (libdrm is not in the Vitis sysroot). Pass an aarch64 sysroot dir
       whose usr/lib/pkgconfig/libdrm.pc exists, or use -cross-yocto if
       you have a Yocto build that already staged libdrm."
    [ -f "$SYSROOT/usr/lib/pkgconfig/libdrm.pc" ] \
        || die "no libdrm.pc under $SYSROOT/usr/lib/pkgconfig/
       (pass -sysroot PATH pointing at a sysroot whose libdrm-dev is staged)"

    export PATH="$VITIS_TC/bin:$PATH"
    export ARCH=arm64
    export CROSS_COMPILE=aarch64-linux-gnu-
    export LOCALVERSION="$LOCALVERSION"

elif [ "$CROSS_YOCTO" -eq 1 ]; then
    [ -d "$YOCTO_BUILD" ] || die "YOCTO_BUILD does not exist: $YOCTO_BUILD"

    # MACHINE auto-detect: pick the only kernel-source dir under work-shared/.
    if [ -z "$YOCTO_MACHINE" ]; then
        shopt -s nullglob
        cands=( "$YOCTO_BUILD"/tmp/work-shared/*/kernel-source )
        shopt -u nullglob
        [ "${#cands[@]}" -gt 0 ] \
            || die "cannot find kernel-source under $YOCTO_BUILD/tmp/work-shared/* (-yocto-machine MACH to override)"
        YOCTO_MACHINE=$(basename "$(dirname "${cands[0]}")")
    fi
    # Yocto uses '-' in MACHINE for some paths, '_' for others.
    M_DASH=${YOCTO_MACHINE//_/-}
    M_USCORE=${YOCTO_MACHINE//-/_}

    KDIR="$YOCTO_BUILD/tmp/work-shared/$M_DASH/kernel-source"
    KOBJ="$YOCTO_BUILD/tmp/work-shared/$M_DASH/kernel-build-artifacts"
    [ -d "$KDIR" ] || die "kernel source not found: $KDIR"
    [ -d "$KOBJ" ] || die "kernel build artifacts not found: $KOBJ"
    [ -f "$KOBJ/Module.symvers" ] \
        || warn "Module.symvers missing in $KOBJ - bitbake the kernel first?"

    # Locate a recipe-sysroot-native with the merged toolchain. Yocto's
    # per-recipe "recipe-sysroot-native" is the merged tree the cross-gcc
    # was actually built against; sysroots-components/ doesn't work
    # because gcc looks for as/ld next to itself via a path that only
    # exists post-merge. Prefer recipes we know exist.
    NATIVE_SYSROOT=""
    for recipe in tessai-agent-mod tessai-agent-lib amdxdna; do
        cand="$YOCTO_BUILD/tmp/work/${M_USCORE}-amd-linux/$recipe"
        [ -d "$cand" ] || continue
        cand=$(ls -d "$cand"/*/recipe-sysroot-native 2>/dev/null | head -1) || true
        if [ -n "$cand" ] && [ -d "$cand" ]; then
            NATIVE_SYSROOT="$cand"
            break
        fi
    done
    if [ -z "$NATIVE_SYSROOT" ]; then
        NATIVE_SYSROOT=$(ls -d \
            "$YOCTO_BUILD"/tmp/work/"${M_USCORE}"-amd-linux/*/*/recipe-sysroot-native \
            2>/dev/null | head -1) || true
    fi
    [ -n "$NATIVE_SYSROOT" ] && [ -d "$NATIVE_SYSROOT" ] \
        || die "no recipe-sysroot-native under $YOCTO_BUILD/tmp/work/${M_USCORE}-amd-linux/*/*/ - bitbake at least one recipe first"

    NATIVE_SYSROOT_BIN="$NATIVE_SYSROOT/usr/bin/aarch64-amd-linux"
    [ -x "$NATIVE_SYSROOT_BIN/aarch64-amd-linux-gcc" ] \
        || die "cross gcc not found in native sysroot: $NATIVE_SYSROOT_BIN/aarch64-amd-linux-gcc"

    # Target sysroot for userspace cmake: prefer the staged image rootfs
    # because that's where libdrm.pc actually lands.
    SYSROOT=""
    ROOTFS=$(ls -d \
        "$YOCTO_BUILD"/tmp/work/"${M_USCORE}"-amd-linux/edf-linux-disk-image/*/rootfs \
        2>/dev/null | head -1) || true
    if [ -n "$ROOTFS" ] && [ -f "$ROOTFS/usr/lib/pkgconfig/libdrm.pc" ]; then
        SYSROOT="$ROOTFS"
    fi
    if [ -z "$SYSROOT" ]; then
        SYSROOT=$(ls -d \
            "$YOCTO_BUILD"/tmp/work/"${M_USCORE}"-amd-linux/*/*/recipe-sysroot \
            2>/dev/null | head -1) || true
    fi
    [ -n "$SYSROOT" ] && [ -d "$SYSROOT" ] \
        || die "no target sysroot found (need edf-linux-disk-image/*/rootfs with libdrm.pc, or any recipe-sysroot)"

    CROSS_CC="$NATIVE_SYSROOT_BIN/aarch64-amd-linux-gcc"
    CROSS_CXX="$NATIVE_SYSROOT_BIN/aarch64-amd-linux-g++"
    CROSS_AR="$NATIVE_SYSROOT_BIN/aarch64-amd-linux-ar"
    CROSS_RANLIB="$NATIVE_SYSROOT_BIN/aarch64-amd-linux-ranlib"

    # PATH order matches Yocto module.bbclass: cross binaries first, then
    # native sysroot's host tools (bison, flex, bc, pahole, openssl,
    # etc.), then the cross-prefixed binaries again.
    export PATH="$NATIVE_SYSROOT_BIN:$NATIVE_SYSROOT/usr/bin:$NATIVE_SYSROOT/usr/sbin:$NATIVE_SYSROOT/sbin:$NATIVE_SYSROOT/bin:$PATH"
    export ARCH=arm64
    export CROSS_COMPILE=aarch64-amd-linux-
    # kbuild needs O=KOBJ because Yocto splits kernel-source / kernel-build-artifacts.
    KBUILD_EXTRA=(
        "O=$KOBJ"
        "LD=aarch64-amd-linux-ld.bfd"
        "AR=aarch64-amd-linux-ar"
        "OBJCOPY=aarch64-amd-linux-objcopy"
        "STRIP=aarch64-amd-linux-strip"
    )

else
    [ -n "$KDIR" ] || KDIR="$NATIVE_KDIR_DEFAULT"
    [ -f "$KDIR/Makefile" ] && [ -d "$KDIR/scripts" ] \
        || die "KDIR is not a kernel build tree: $KDIR
       (install linux-headers-\$(uname -r) or pass -kdir PATH)"
fi

mkdir -p "$OUT/driver" "$OUT/lib" "$OUT/bin"
log "tessai-agent build"
if [ "$CROSS_VITIS" -eq 1 ]; then
    log "  Mode:         cross-vitis aarch64"
elif [ "$CROSS_YOCTO" -eq 1 ]; then
    log "  Mode:         cross-yocto aarch64"
else
    log "  Mode:         native"
fi
log "  Repo:         $REPO"
log "  Output:       $OUT"
log "  KDIR:         $KDIR"
[ -n "$KOBJ" ]    && log "  KOBJ:         $KOBJ"
if [ "$CROSS_VITIS" -eq 1 ]; then
    log "  Toolchain:    $VITIS_TC"
    log "  Compiler:     $("$CROSS_CC" --version | head -1)"
    log "  LOCALVERSION: '$LOCALVERSION'"
elif [ "$CROSS_YOCTO" -eq 1 ]; then
    log "  Yocto build:  $YOCTO_BUILD"
    log "  Machine:      $YOCTO_MACHINE"
    log "  Native sys.:  $NATIVE_SYSROOT_BIN"
    log "  Compiler:     $("$CROSS_CC" --version 2>&1 | head -1)"
fi
[ -n "$SYSROOT" ] && log "  Sysroot:      $SYSROOT"
log "  Jobs:         $JOBS"
echo

# ---------- kernel module phase ----------
build_kmod() {
    log "Building tessai-agent kernel module"

    # cp -rs: directories real, files symlinked. kbuild writes its outputs
    # (.o, .ko, .mod*, .cmd, Module.symvers, modules.order) as real files
    # next to the source symlinks in the mirror, never touching the source.
    # Both drivers/ and include/ must be mirrored, because Kbuild does
    # `-I$(src)/../../../include/uapi`.
    rm -rf "$OUT/obj"
    mkdir -p "$OUT/obj"
    cp -rs "$REPO/drivers" "$OUT/obj/"
    cp -rs "$REPO/include" "$OUT/obj/"

    local MIRROR_M="$OUT/obj/drivers/accel/tessai-agent"

    # Defensive: if the source tree had stale build artifacts (e.g. from
    # a previous in-source `make -C drivers/accel/tessai-agent`), they
    # show up as symlinks in the mirror. Delete them so kbuild creates
    # fresh real files in their place rather than writing through a
    # symlink back into the source.
    find "$MIRROR_M" -maxdepth 1 \
        \( -name '*.ko' -o -name '*.o' -o -name '*.mod' \
           -o -name '*.mod.*' -o -name '.*.cmd' \
           -o -name 'Module.symvers' -o -name 'modules.order' \) -delete

    # KBUILD_MODPOST_WARN=1 turns modpost's "undefined symbol" errors into
    # warnings; required when KDIR's Module.symvers is empty (common for
    # shared prepared trees that were never fully built). Symbols still
    # resolve at load time on the running kernel.
    KBUILD_MODPOST_WARN=1 \
        make -C "$KDIR" M="$MIRROR_M" -j "$JOBS" \
             "${KBUILD_EXTRA[@]}" modules 2>&1 \
            | tee "$OUT/build-kmod.log" \
            | grep -E '^(  CC|  LD|  MODPOST|ERROR|make.*Error|WARNING)' \
            || true

    [ -f "$MIRROR_M/tessai_agent.ko" ] \
        || die "tessai_agent.ko was not produced (see $OUT/build-kmod.log)"

    cp -f "$MIRROR_M/tessai_agent.ko" "$OUT/driver/"

    if [ "$CROSS" -eq 1 ] && [ "$STRIP_DIRTY" -eq 1 ]; then
        python3 "$STRIP_DIRTY_PY" \
            "$OUT/driver/tessai_agent.ko" >"$OUT/strip-dirty.log" 2>&1 \
            || { cat "$OUT/strip-dirty.log" >&2; die "vermagic strip failed"; }
    fi

    log "  -> $OUT/driver/tessai_agent.ko"
    /usr/sbin/modinfo "$OUT/driver/tessai_agent.ko" 2>/dev/null \
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
        -DTESSAI_AGENT_LIB_BUILD_EXAMPLES=ON
    )
    local cmake_env=()

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
            -DTESSAI_AGENT_LINK_EXAMPLES_STATIC=ON
        )
        cmake_env+=(
            "PKG_CONFIG_SYSROOT_DIR=$SYSROOT"
            "PKG_CONFIG_LIBDIR=$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig"
            "PKG_CONFIG_PATH=$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig"
        )
        # In cross-yocto, prefer the native sysroot's pkg-config so it
        # honors the SYSROOT_DIR rewrite consistently.
        [ -n "$NATIVE_SYSROOT_BIN" ] \
            && cmake_env+=( "PKG_CONFIG=${NATIVE_SYSROOT_BIN%/aarch64-amd-linux}/pkg-config" )
    fi

    env "${cmake_env[@]}" cmake "${cmake_args[@]}" >"$OUT/cmake-configure.log" 2>&1 \
        || { cat "$OUT/cmake-configure.log" >&2; die "cmake configure failed"; }
    env "${cmake_env[@]}" cmake --build "$OUT/cmake-lib" -j "$JOBS" 2>&1 \
        | tee "$OUT/cmake-build.log" \
        | grep -E '^\[|error:|Error|warning:' || true

    shopt -s nullglob
    local libs=( "$OUT/cmake-lib"/libtessai_agent.so* "$OUT/cmake-lib"/libtessai_agent.a )
    shopt -u nullglob
    [ "${#libs[@]}" -gt 0 ] \
        || die "no libtessai_agent.* produced (see $OUT/cmake-build.log)"
    cp -af "${libs[@]}" "$OUT/lib/"

    # Copy every example listed in lib/CMakeLists.txt _tess_examples into
    # bin/. Kept as an explicit list (not a glob over cmake-lib/) so that
    # a renamed or deleted example fails the build loudly with the
    # "was not produced" die instead of silently dropping out of the
    # artifact set. Must stay in lockstep with _tess_examples in
    # lib/CMakeLists.txt.
    local examples=( tessai_agent_test tessai_agent_inout_example )
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
# Build tests/unittests/tessai_agent_unittest (Link A acceptance
# suite). The parent tests/ CMake project itself only contains the
# gate flag (TESSAI_AGENT_BUILD_UNITTESTS) and the add_subdirectory
# call -- there is no longer a standalone C smoke binary, so without
# -unit_tests the tests/ phase is effectively a no-op configure pass
# kept solely for symmetry with -no-tests.
#
# The gtest binary itself is libdrm-free -- it only depends on the
# kernel UAPI headers (drm/, linux/sync_file) and this repo's
# include/uapi/tessai-agent/tessai_agent.h, plus a C++17 toolchain
# (libstdc++ <chrono>/<thread>/<atomic>/<filesystem>). In cross
# modes we still need a SYSROOT to find the kernel UAPI headers.
build_tests() {
    [ "$BUILD_TESTS" -eq 1 ] || { log "Skipping tests/ (-no-tests)"; return 0; }
    [ -f "$REPO/tests/CMakeLists.txt" ] \
        || { warn "tests/CMakeLists.txt not present, skipping"; return 0; }

    if [ "$BUILD_UNITTESTS" -ne 1 ]; then
        log "tests/ configured for unittests-OFF (no-op without -unit_tests)"
        return 0
    fi

    log "Building Link A gtest suite (tests/unittests/)"

    local cmake_args=(
        -S "$REPO/tests"
        -B "$OUT/cmake-tests"
        -DCMAKE_BUILD_TYPE=Release
        -DTESSAI_AGENT_BUILD_UNITTESTS=ON
        # Auto-stage on every link so incremental rebuilds
        # (`make -C $OUT/cmake-tests/unittests`) keep $OUT/bin/
        # in sync without needing a full ./build/build.sh
        # re-run. boot_vek385.py reads $OUT/bin/, so a stale
        # staged copy is an easy way to chase a "but I just fixed
        # that!" ghost (the new binary builds, the OLD one runs).
        -DTESSAI_AGENT_STAGE_BIN_DIR="$OUT/bin"
    )
    local cmake_env=()

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
        )
        [ -x "$CROSS_CXX" ] \
            || die "-unit_tests requires a cross g++ at $CROSS_CXX"
        cmake_args+=( -DCMAKE_CXX_COMPILER="$CROSS_CXX" )
    fi

    env "${cmake_env[@]}" cmake "${cmake_args[@]}" >"$OUT/cmake-tests-configure.log" 2>&1 \
        || { cat "$OUT/cmake-tests-configure.log" >&2; die "tests cmake configure failed"; }
    env "${cmake_env[@]}" cmake --build "$OUT/cmake-tests" -j "$JOBS" 2>&1 \
        | tee "$OUT/cmake-tests-build.log" \
        | grep -E '^\[|error:|Error|warning:' || true

    if [ -f "$OUT/cmake-tests/unittests/tessai_agent_unittest" ]; then
        cp -f "$OUT/cmake-tests/unittests/tessai_agent_unittest" "$OUT/bin/"
        log "  -> $OUT/bin/tessai_agent_unittest"
    else
        die "tessai_agent_unittest was not produced (see $OUT/cmake-tests-build.log). \
Did you run: git submodule update --init --recursive ?"
    fi

    # Surface the bundled RPURuntime firmware path so users / CI know
    # where to grab the Link A reference image to flash on remoteproc1.
    if [ -f "$REPO/tests/rpu_app/zephyr/build/zephyr/zephyr.elf" ]; then
        log "  Bundled RPU firmware: $REPO/tests/rpu_app/zephyr/build/zephyr/zephyr.elf"
    else
        warn "  tests/rpu_app/zephyr/build/zephyr/zephyr.elf is missing"
        warn "  (rebuild it from tests/rpu_app/ via: cd tests/rpu_app && ./build.sh tests/tessai_agent_test)"
    fi
    echo
}

# ---------- run ----------
build_kmod
build_lib
build_tests

log "Done. Artifacts under $OUT/:"
log "  driver/tessai_agent.ko"
for f in $(ls -1 "$OUT/lib/"); do log "  lib/$f"; done
for f in $(ls -1 "$OUT/bin/"); do log "  bin/$f"; done
