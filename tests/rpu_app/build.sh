#!/bin/bash

# Zephyr AMD Build Script
# Usage: ./build.sh <test_case_name>
#        ./build.sh clean
# Example: ./build.sh tests/xrt_test

# Ensure script runs in bash (re-execute with bash if not)
if [ -z "$BASH_VERSION" ]; then
    # Not running in bash, re-execute with bash
    exec bash "$0" "$@"
fi

set -e  # Exit on error

# ---------------------------------------------------------------------------
# Pinned tool versions
#
# RPURuntime tracks the Xilinx zephyr-amd branch xlnx_rel_v2025.2, which
# is Zephyr 3.7.0. That release pins the SDK to 0.16.8 (see
# zephyr/SDK_VERSION) and requires Python >= 3.8 (see
# zephyr/cmake/modules/python.cmake PYTHON_MINIMUM_REQUIRED).
#
# Ubuntu 22.04 ships python3.10 which is fine for Zephyr 3.7. We still
# resolve via the `uv`/python3.12 path below so the venv works on hosts
# where python3.10 is unavailable, but anything >= 3.8 is acceptable.
#
# SDK 0.16.x layout differs from 1.0.x:
#   - URL: toolchain_linux-x86_64_*.tar.xz       (no `gnu_` prefix)
#   - Extract: toolchain extracts to ${SDK}/arm-zephyr-eabi/
#              (not ${SDK}/gnu/arm-zephyr-eabi/ as in 1.0.x).
#
# Bump these in one place if Zephyr's requirements move (and update
# the extract layout in step 8 if jumping to a 1.0.x line).
# ---------------------------------------------------------------------------
ZEPHYR_SDK_VERSION="0.16.8"
ZEPHYR_SDK_DIR="zephyr-sdk-${ZEPHYR_SDK_VERSION}"
ZEPHYR_SDK_MINIMAL_URL="https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${ZEPHYR_SDK_VERSION}/zephyr-sdk-${ZEPHYR_SDK_VERSION}_linux-x86_64_minimal.tar.xz"
ZEPHYR_SDK_ARM_TOOLCHAIN_URL="https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${ZEPHYR_SDK_VERSION}/toolchain_linux-x86_64_arm-zephyr-eabi.tar.xz"
ZEPHYR_SDK_ARM_TOOLCHAIN_NAME="arm-zephyr-eabi"

# Lower bound for the Python interpreter we'll use for the venv. Zephyr
# 3.7 only requires 3.8; 3.12 is safe (>= 3.8) and is what the
# user-local `uv`-managed interpreter happens to install.
PYTHON_MIN_VERSION="3.8"

# Resolve a Python >= ${PYTHON_MIN_VERSION} interpreter without sudo.
# Order: existing PATH (python3.12/.13), then uv-installed standalone
# Python, then bootstrap uv itself into ~/.local/bin and install 3.12.
# Echoes the absolute path on stdout.
resolve_python() {
    local p
    for p in python3.12 python3.13; do
        if command -v "$p" >/dev/null 2>&1; then
            echo "$(command -v "$p")"
            return 0
        fi
    done
    if ! command -v uv >/dev/null 2>&1; then
        # User-local install; no sudo, no system mutation.
        curl -LsSf --max-time 60 https://astral.sh/uv/install.sh | bash >&2
        export PATH="$HOME/.local/bin:$PATH"
    fi
    if ! command -v uv >/dev/null 2>&1; then
        echo "ERROR: failed to install 'uv'; install Python ${PYTHON_MIN_VERSION}+ manually." >&2
        return 1
    fi
    uv python install "${PYTHON_MIN_VERSION}" >&2 || true
    uv python find "${PYTHON_MIN_VERSION}"
}

# Check for clean command first (before logging setup)
if [ "$1" = "clean" ]; then
    echo "========================================="
    echo "Cleaning Build Environment"
    echo "========================================="
    echo ""
    echo "This will remove all directories created during the build process:"
    echo "  • .venv (Python virtual environment)"
    echo "  • .west (West workspace config)"
    echo "  • .Xil (Xilinx temporary files)"
    echo "  • zephyr (Xilinx Zephyr repository)"
    echo "  • zephyr.upstream (Backup repository)"
    echo "  • ${ZEPHYR_SDK_DIR} (Zephyr SDK)"
    echo "  • modules (West modules)"
    echo "  • bootloader (West bootloader)"
    echo "  • tools (Build tools)"
    echo "  • lopper (Device tree tool)"
    echo "  • output_dir (Generated device trees)"
    echo "  • build (Build artifacts)"
    echo "  • Downloaded SDK archives (tar.xz files)"
    echo ""
    echo "The following will NOT be removed:"
    echo "  • xrt/ (XRT Lite library source)"
    echo "  • tests/ (test applications)"
    echo "  • platform/dts/ (board Device Tree fragments, e.g. reserved.dtsi)"
    echo "  • platform/ (platform files, e.g. example_design_pfm_fixed.xsa)"
    echo "  • build.sh, README.md, LICENSE"
    echo ""

    CLEANED=0

    # Remove Python virtual environment
    if [ -d ".venv" ]; then
        echo "Removing .venv directory..."
        rm -rf .venv
        echo "✓ .venv removed"
        CLEANED=1
    fi

    # Remove West workspace
    if [ -d ".west" ]; then
        echo "Removing .west directory..."
        rm -rf .west
        echo "✓ .west removed"
        CLEANED=1
    fi

    # Remove Xilinx temporary files
    if [ -d ".Xil" ]; then
        echo "Removing .Xil directory..."
        rm -rf .Xil
        echo "✓ .Xil removed"
        CLEANED=1
    fi

    # Remove Zephyr repositories
    if [ -d "zephyr" ]; then
        echo "Removing zephyr directory..."
        rm -rf zephyr
        echo "✓ zephyr removed"
        CLEANED=1
    fi

    if [ -d "zephyr.upstream" ]; then
        echo "Removing zephyr.upstream directory..."
        rm -rf zephyr.upstream
        echo "✓ zephyr.upstream removed"
        CLEANED=1
    fi

    # Remove Zephyr SDK(s) - current pinned version plus any stale older
    # installs and the .bak directory left behind by previous in-place
    # upgrades.
    for sdk in "${ZEPHYR_SDK_DIR}" zephyr-sdk-*; do
        if [ -d "$sdk" ]; then
            echo "Removing $sdk directory..."
            rm -rf "$sdk"
            echo "✓ $sdk removed"
            CLEANED=1
        fi
    done

    # Remove West modules
    if [ -d "modules" ]; then
        echo "Removing modules directory..."
        rm -rf modules
        echo "✓ modules removed"
        CLEANED=1
    fi

    if [ -d "bootloader" ]; then
        echo "Removing bootloader directory..."
        rm -rf bootloader
        echo "✓ bootloader removed"
        CLEANED=1
    fi

    # Remove tools directory
    if [ -d "tools" ]; then
        echo "Removing tools directory..."
        rm -rf tools
        echo "✓ tools removed"
        CLEANED=1
    fi

    # Remove lopper directory
    if [ -d "lopper" ]; then
        echo "Removing lopper directory..."
        rm -rf lopper
        echo "✓ lopper removed"
        CLEANED=1
    fi

    # Remove output directory
    if [ -d "output_dir" ]; then
        echo "Removing output_dir directory..."
        rm -rf output_dir
        echo "✓ output_dir removed"
        CLEANED=1
    fi

    # Remove build directory
    if [ -d "build" ]; then
        echo "Removing build directory..."
        rm -rf build
        echo "✓ build removed"
        CLEANED=1
    fi

    # Remove downloaded SDK archives (any version + the arm toolchain
    # tarball pulled in alongside the minimal SDK in step 8).
    for arc in zephyr-sdk-*_linux-x86_64*.tar.xz toolchain_gnu_linux-x86_64_*-zephyr-*.tar.xz; do
        if [ -f "$arc" ]; then
            echo "Removing SDK archive $arc..."
            rm -f "$arc"
            echo "✓ $arc removed"
            CLEANED=1
        fi
    done

    # Remove log file
    if [ -f "output_log.txt" ]; then
        echo "Removing output_log.txt..."
        rm -f output_log.txt
        echo "✓ output_log.txt removed"
        CLEANED=1
    fi

    echo ""
    echo "========================================="
    if [ $CLEANED -eq 1 ]; then
        echo "✓ Cleanup completed successfully!"
        echo "All build artifacts removed."
    else
        echo "✓ Nothing to clean - environment already clean"
    fi
    echo "========================================="
    exit 0
fi

# Setup logging
LOG_FILE="output_log.txt"
exec > >(tee -a "$LOG_FILE") 2>&1

# Function to show progress spinner
show_spinner() {
    local pid=$1
    local message=$2
    local spin='-\|/'
    local i=0
    echo -n "$message "
    while kill -0 $pid 2>/dev/null; do
        i=$(( (i+1) %4 ))
        printf "\r$message ${spin:$i:1}"
        sleep 0.1
    done
    printf "\r$message ✓\n"
}

# Function to run command with progress indicator
run_with_progress() {
    local message=$1
    shift
    echo "$message"
    echo "⏳ This may take a few minutes, please wait..."
    "$@"
    echo "✓ Completed"
    echo ""
}

echo "========================================="
echo "Build started at: $(date)"
echo "Log file: $LOG_FILE"
echo "========================================="

# Check arguments
if [ $# -ne 1 ]; then
    echo "Error: Missing required arguments"
    echo ""
    echo "Usage:"
    echo "  $0 <test_case_name>              - Smart build (auto-detects environment)"
    echo "  $0 clean                         - Remove all build artifacts"
    echo ""
    echo "Examples:"
    echo "  $0 tests/xrt_test"
    echo "  $0 clean"
    exit 1
fi

SCRIPT_PATH="${BASH_SOURCE[0]}"
SCRIPT_DIR="$(cd "$(dirname "$SCRIPT_PATH")" && pwd)"
XSA_FILE=$(realpath "$SCRIPT_DIR/platform/example_design_pfm_fixed.xsa")
TEST_CASE="$1"

# Verify XSA file exists
if [ ! -f "$XSA_FILE" ]; then
    echo "Error: XSA file '$XSA_FILE' not found"
    exit 1
fi

# Get absolute path
XSA_FILE=$(realpath "$XSA_FILE")

echo "========================================="
echo "Zephyr AMD Build Script"
echo "========================================="
echo "XSA File: $XSA_FILE"
echo "Test Case: $TEST_CASE"
echo "========================================="

# Check if environment is fully set up.
#
# We require ALL of:
#   - .venv                Python 3.12 venv (step 2)
#   - .west                west workspace metadata (step 4)
#   - zephyr/              Xilinx zephyr-amd tree cloned + west-updated
#                          (step 9)
#   - ${ZEPHYR_SDK_DIR}    SDK 1.0.1 unpacked + registered (step 8)
#   - lopper/              lopper sources cloned by `west update`
#                          (step 9 manifest project)
#   - output_dir/          SDT generated from XSA + lopper-patched
#                          versal2_rpu.dts (step 11). reserved.dtsi
#                          references labels lopper injects
#                          (ipi_2_to_ipi_1, ps_wizard_0_ps11_0_ocm_*),
#                          so without this the DT parse fails.
#
# Re-runs after a partial setup are safe: every step 1-11 is
# individually idempotent and will skip its work if the artifact is
# already present.
SKIP_TO_BUILD=0
if [ -d ".venv" ] && [ -d ".west" ] && [ -d "zephyr" ] \
        && [ -d "${ZEPHYR_SDK_DIR}" ] \
        && [ -d "lopper" ] \
        && [ -d "output_dir" ]; then
    echo ""
    echo "╔════════════════════════════════════════════════════════════════╗"
    echo "║          ENVIRONMENT DETECTED - FAST BUILD MODE                ║"
    echo "╚════════════════════════════════════════════════════════════════╝"
    echo ""
    echo "All dependencies are present:"
    echo "  ✓ .venv (Python virtual environment)"
    echo "  ✓ .west (West workspace)"
    echo "  ✓ zephyr (Zephyr repository)"
    echo "  ✓ ${ZEPHYR_SDK_DIR} (SDK installed)"
    echo "  ✓ lopper + output_dir (SDT generated from XSA)"
    echo ""
    echo "Skipping steps 1-11, jumping directly to build (Step 12-13)..."
    echo "⏱️  This will be much faster (~30 seconds - 2 minutes)"
    echo ""
    echo "To force full setup, run: ./build.sh clean && ./build.sh $TEST_CASE"
    echo ""
    SKIP_TO_BUILD=1
fi

if [ $SKIP_TO_BUILD -eq 0 ]; then
    echo ""
    echo "Setting up build environment (first time: 15-25 minutes)..."
    echo ""

# Step 1: Verify pre-installed dependencies
echo "[1/13] Verifying pre-installed dependencies..."
echo ""
echo "Checking that required system packages are available..."
echo "(Packages must be pre-installed on the build machine.)"
echo ""

MISSING_TOOLS=""
for cmd in cmake python3 dtc ninja git wget ccache make gcc g++ file rsync; do
    if command -v $cmd &>/dev/null; then
        echo "  ✓ $cmd ($(command -v $cmd))"
    else
        echo "  ✗ $cmd NOT FOUND"
        MISSING_TOOLS="$MISSING_TOOLS $cmd"
    fi
done

if [ -n "$MISSING_TOOLS" ]; then
    echo ""
    echo "╔════════════════════════════════════════════════════════════════╗"
    echo "║              MISSING DEPENDENCIES                              ║"
    echo "╚════════════════════════════════════════════════════════════════╝"
    echo ""
    echo "ERROR: The following required tools are not installed:$MISSING_TOOLS"
    echo ""
    echo "Please install them on the build machine before running this script:"
    echo ""
    echo "  sudo apt install --no-install-recommends git cmake ninja-build \\"
    echo "    gperf ccache dfu-util device-tree-compiler wget python3-dev \\"
    echo "    python3-pip python3-setuptools python3-tk python3-wheel \\"
    echo "    xz-utils file make gcc gcc-multilib g++-multilib \\"
    echo "    libsdl2-dev libmagic1 python3-venv"
    echo ""
    exit 1
fi

echo ""
echo "Verifying installed versions:"
echo "  CMake version: $(cmake --version | head -n1)"
echo "  Python3 version: $(python3 --version)"
echo "  DTC version: $(dtc --version 2>&1 | head -n1)"
echo ""
echo "✓ All dependencies verified"
echo ""

# Step 2: Setup Python virtual environment
#
# Zephyr 4.4 enforces Python >= ${PYTHON_MIN_VERSION} in
# zephyr/cmake/modules/python.cmake.  Ubuntu 22.04's system python3 is
# 3.10, which fails the CMake check, so we resolve (and if necessary
# install via `uv`) a standalone 3.12 interpreter before creating the
# venv.  resolve_python() never asks for sudo - everything lands in
# ~/.local.  If a venv already exists but was built with an older
# Python, rebuild it.
echo "[2/13] Setting up Python virtual environment..."
PYTHON_FOR_VENV="$(resolve_python)"
if [ -z "$PYTHON_FOR_VENV" ] || [ ! -x "$PYTHON_FOR_VENV" ]; then
    echo "ERROR: could not locate a Python >= ${PYTHON_MIN_VERSION} interpreter."
    exit 1
fi
echo "Using interpreter: $PYTHON_FOR_VENV ($("$PYTHON_FOR_VENV" --version))"

if [ -d ".venv" ]; then
    VENV_PYVER="$(.venv/bin/python --version 2>&1 | awk '{print $2}')"
    VENV_MAJOR_MINOR="${VENV_PYVER%.*}"
    if ! printf '%s\n%s\n' "${PYTHON_MIN_VERSION}" "${VENV_MAJOR_MINOR}" \
            | sort -V -C; then
        echo "Existing .venv is Python ${VENV_PYVER} (< ${PYTHON_MIN_VERSION}); rebuilding..."
        rm -rf .venv
    fi
fi

if [ ! -d ".venv" ]; then
    echo "Creating Python virtual environment with $PYTHON_FOR_VENV..."
    "$PYTHON_FOR_VENV" -m venv .venv
    echo "✓ Virtual environment created"
else
    echo "✓ Virtual environment already exists ($(.venv/bin/python --version)), reusing"
fi
source .venv/bin/activate
echo "✓ Virtual environment activated"
echo ""

# Step 3: Install west
echo "[3/13] Installing west..."
if ! command -v west &> /dev/null; then
    echo "⏳ Installing Python west tool..."
    pip install --no-cache-dir west
    echo "✓ West installed"
else
    echo "✓ West already installed, skipping"
fi
echo ""

# Step 4: Initialize west workspace
echo "[4/13] Initializing west workspace..."
if [ ! -f ".west/config" ]; then
    echo "Initializing west workspace..."
    west init .
    echo "✓ West workspace initialized"
else
    echo "✓ West workspace already initialized, skipping"
fi
echo ""

# Step 5: Update west
echo "[5/13] Updating west projects..."
if [ ! -d "modules" ] || [ ! -d "bootloader" ]; then
    echo "⏳ Fetching and updating all west modules..."
    echo "   This can take 5-10 minutes depending on network speed..."
    echo "   Downloading Zephyr modules and dependencies..."
    west update
    echo "✓ West update completed"
else
    echo "✓ West modules already downloaded, skipping update"
fi
echo ""

# Step 6: Export Zephyr
echo "[6/13] Exporting Zephyr environment..."
west zephyr-export
echo "✓ Zephyr environment exported"
echo ""

# Step 7: Install Zephyr requirements
echo "[7/13] Installing Zephyr requirements..."
if [ -f "./zephyr/scripts/requirements.txt" ]; then
    echo "⏳ Installing Python dependencies from requirements.txt..."
    pip install --no-cache-dir -r ./zephyr/scripts/requirements.txt
    echo "✓ Python requirements installed"
else
    echo "✓ Requirements file not found, skipping"
fi
echo ""

# Step 8: Download and setup Zephyr SDK (if not already done)
#
# Zephyr 3.7 (xlnx_rel_v2025.2) pins to SDK 0.16.8 (zephyr/SDK_VERSION).
# We pull only the minimal SDK tarball (~70 MB - host CMake glue, no
# toolchains) plus the arm-zephyr-eabi toolchain (~100 MB - the only
# one versal2_rpu needs; it's a 32-bit Cortex-R52). Total ~170 MB vs
# ~1.5 GB for the full bundle.
#
# SDK 0.16.x layout: toolchain extracts to ${SDK}/arm-zephyr-eabi/
# (top-level). SDK 1.0.x moved this under ${SDK}/gnu/. If you bump
# ZEPHYR_SDK_VERSION into the 1.0 line, switch back to the gnu/ path.
#
# setup.sh is called with `-c` only: register the CMake package and do
# nothing else.  The historical interactive flow tried to install host
# tools (`-h`) which requires sudo / out-of-band downloads and breaks on
# fresh clones.
echo "[8/13] Checking Zephyr SDK..."
if [ ! -d "${ZEPHYR_SDK_DIR}" ]; then
    SDK_ARC="$(basename "${ZEPHYR_SDK_MINIMAL_URL}")"
    TC_ARC="$(basename "${ZEPHYR_SDK_ARM_TOOLCHAIN_URL}")"

    if [ ! -f "${SDK_ARC}" ]; then
        echo "⏳ Downloading Zephyr SDK ${ZEPHYR_SDK_VERSION} minimal (~70 MB)..."
        wget -q --show-progress "${ZEPHYR_SDK_MINIMAL_URL}" -O "${SDK_ARC}"
    fi
    if [ ! -f "${TC_ARC}" ]; then
        echo "⏳ Downloading ${ZEPHYR_SDK_ARM_TOOLCHAIN_NAME} toolchain (~100 MB)..."
        wget -q --show-progress "${ZEPHYR_SDK_ARM_TOOLCHAIN_URL}" -O "${TC_ARC}"
    fi

    echo "⏳ Extracting SDK archive..."
    tar xJf "${SDK_ARC}"

    echo "⏳ Extracting ${ZEPHYR_SDK_ARM_TOOLCHAIN_NAME} toolchain into ${ZEPHYR_SDK_DIR}/..."
    tar xJf "${TC_ARC}" -C "${ZEPHYR_SDK_DIR}"

    echo "⏳ Registering SDK with CMake (non-interactive, no host tools)..."
    (cd "${ZEPHYR_SDK_DIR}" && ./setup.sh -c)

    echo "✓ Zephyr SDK ${ZEPHYR_SDK_VERSION} installed and registered"
else
    echo "✓ Zephyr SDK ${ZEPHYR_SDK_VERSION} already installed"
    # Re-register in case a different SDK is currently the active one
    # in ~/.cmake/packages/Zephyr-sdk/ (cheap, idempotent).
    (cd "${ZEPHYR_SDK_DIR}" && ./setup.sh -c >/dev/null)
fi
echo ""

# Step 9: Clone Xilinx Zephyr repository
echo "[9/13] Setting up Xilinx Zephyr..."
if [ -d "zephyr" ] && [ ! -d "zephyr.upstream" ]; then
    mv zephyr zephyr.upstream
fi

if [ ! -d "zephyr" ]; then
    echo "⏳ Cloning Xilinx Zephyr AMD repository..."
    echo "   Fetching xlnx_rel_v2025.2 branch..."
    git clone https://github.com/Xilinx/zephyr-amd.git -b xlnx_rel_v2025.2 zephyr
    echo "✓ Repository cloned"

    echo "⏳ Updating west modules for Xilinx Zephyr..."
    echo "   This may take a few minutes..."
    west update
    echo "✓ West update completed"
else
    echo "✓ Xilinx Zephyr repository already exists"
fi

# zephyr/west.yml lists `lopper` as a manifest project that west
# clones into ${WEST_TOPDIR}/lopper. If that directory is missing,
# the previous `west update` was either never run or didn't finish
# (e.g. interrupted partial setup). Re-run it — west update is
# idempotent and is a noop for projects already at the pinned SHA.
if [ ! -d "lopper" ]; then
    echo "⏳ lopper/ missing — running 'west update' to populate manifest projects..."
    echo "   (noop for any project already at the pinned SHA)"
    west update
    echo "✓ West update completed"
fi
echo ""

# Step 10: Install lopper.
#
# `west lopper-install` (zephyr/scripts/west_commands/lopper_install.py)
# pip-installs lopper from ${WEST_TOPDIR}/lopper, which must already
# have been cloned by `west update` (step 9). The previous gate
# checked .west/lopper which is the wrong path and never matches,
# so this used to silently re-pip-install on every full setup.
#
# Detect a successful install by probing the venv for the `lopper`
# Python module rather than a stray directory.
echo "[10/13] Installing lopper..."
cd zephyr
if ! python -c "import lopper" >/dev/null 2>&1; then
    if [ ! -d "../lopper" ]; then
        echo "❌ ../lopper missing even after step 9. Re-run ./build.sh; if"
        echo "   the problem persists, manually run 'west update' from"
        echo "   ${SCRIPT_DIR}."
        exit 1
    fi
    echo "⏳ Installing lopper tool for device tree processing..."
    west lopper-install
    echo "✓ Lopper installed"
else
    echo "✓ Lopper already installed (importable), skipping"
fi
cd ../
echo ""

# Step 11: Copy XSA file and generate SDT
echo "[11/13] Generating System Device Tree from XSA..."
XSA_BASENAME=$(basename "$XSA_FILE")

# Only copy if XSA file is not already in current directory
XSA_DIR=$(dirname "$XSA_FILE")
CWD=$(realpath ".")
if [ "$XSA_DIR" != "$CWD" ]; then
    echo "Copying XSA file to working directory..."
    cp "$XSA_FILE" .
else
    echo "XSA file already in working directory, skipping copy"
fi

# Source Vitis settings if available
if [ -f "/proj/xbuilds/2025.2_daily_latest/installs/lin64/2025.2/Vitis/settings64.sh" ]; then
    source /proj/xbuilds/2025.2_daily_latest/installs/lin64/2025.2/Vitis/settings64.sh
elif [ ! -z "$XILINX_VITIS" ]; then
    echo "Using existing Vitis environment: $XILINX_VITIS"
else
    echo "Warning: Vitis environment not found. Please source settings64.sh manually if needed."
fi

# Generate SDT from XSA
echo "⏳ Generating System Device Tree from XSA file..."
echo "   Running xsct sdtgen command..."
xsct -eval "sdtgen set_dt_param -dir output_dir -xsa $XSA_BASENAME ; sdtgen generate_sdt"
echo "✓ SDT generated in output_dir/"
echo ""

# Run lopper command
echo "⏳ Running lopper to process device tree for cortexr52_2..."
echo "   Processing system-top.dts and generating board configuration..."
LOPPER_DTC_FLAGS="-b 0 -@" west lopper-command -p cortexr52_2 -s output_dir/system-top.dts -w zephyr
echo "✓ Lopper processing completed"
echo ""

else
    # Environment exists - close the if block from setup section
    echo "Proceeding to build..."
    echo ""
fi

# Step 12: Build the test case (always executed)
echo "[12/13] Building test case: $TEST_CASE"

# Activate virtual environment if not already active
if [ -z "$VIRTUAL_ENV" ]; then
    source .venv/bin/activate
    echo "✓ Virtual environment activated"
fi

# Sync xrt/ and tests/ into zephyr/ — only modified files are updated
# so that incremental builds work correctly
if [ -d "${SCRIPT_DIR}/xrt" ]; then
    echo "Syncing xrt into zephyr/xrt/..."
    mkdir -p zephyr/xrt
    rsync -av --delete ${SCRIPT_DIR}/xrt/ zephyr/xrt/
    echo "✓ xrt synced"
fi
if [ -d "${SCRIPT_DIR}/tests" ]; then
    echo "Syncing tests into zephyr/tests/..."
    mkdir -p zephyr/tests
    rsync -av --delete ${SCRIPT_DIR}/tests/ zephyr/tests/
    echo "✓ tests synced"
fi

# Merge reserved-memory fragment into the Zephyr board DTS (from lopper)
if [ -f "${SCRIPT_DIR}/platform/dts/reserved.dtsi" ]; then
    BOARD_DTS_DIR="zephyr/boards/amd/versal2_rpu"
    BOARD_DTS="${BOARD_DTS_DIR}/versal2_rpu.dts"
    if [ -f "$BOARD_DTS" ]; then
        cp -f "${SCRIPT_DIR}/platform/dts/reserved.dtsi" "${BOARD_DTS_DIR}/"
        # Include at end of board DTS so other labels are defined first.
        grep -vF '#include "reserved.dtsi"' "$BOARD_DTS" > "${BOARD_DTS}.tmp" &&
            mv "${BOARD_DTS}.tmp" "$BOARD_DTS"
        printf '\n#include "reserved.dtsi"\n' >> "$BOARD_DTS"
        echo "✓ reserved.dtsi #include appended to end of board DTS"
    else
        echo "Warning: $BOARD_DTS not found; skipping reserved.dtsi"
    fi
fi

# Point the SoC Kconfig at the actual RPU cpu node lopper emitted.
#
# zephyr/soc/xlnx/versal2/Kconfig.defconfig has, in the
# `if SOC_AMD_VERSAL2_RPU` block:
#
#   config SYS_CLOCK_HW_CYCLES_PER_SEC
#       default $(dt_node_int_prop_int,/cpus/cpu@0,clock-frequency)
#
# That `/cpus/cpu@0` path was correct only when lopper targets
# cortexr52_0. We invoke `west lopper-command -p cortexr52_2`
# (step 11), which deletes cpu@0 and emits the active core as
# cortexr52_2: cpu@2. With no /cpus/cpu@0 the Kconfig default falls
# through to 0 and the build dies at sys_clock.h:151
#   #error "SYS_CLOCK_HW_CYCLES_PER_SEC must be non-zero!"
#
# Rewrite the path to /cpus/cpu@2 INSIDE the SOC_AMD_VERSAL2_RPU
# block only (the SOC_AMD_VERSAL2_APU block in the same file uses
# the same hardcoded cpu@0 path but is irrelevant to us; leave it
# untouched so APU builds still work for whoever needs them).
#
# This keeps the clock rate sourced from DT — lopper writes
# clock-frequency = <0x5f5dd19> (99_999_001 Hz) into cpu@2 from
# the XSA, and Kconfig now reads that same value. If a future XSA
# changes the PS11 clock, no Kconfig touch is needed; just the path
# stays synced to whichever R52 core we lopper for. If we ever
# switch to `-p cortexr52_0` or `-p cortexr52_1`, update the
# substitution target below.
SOC_KCONFIG="zephyr/soc/xlnx/versal2/Kconfig.defconfig"
if [ -f "$SOC_KCONFIG" ]; then
    if grep -q '^if SOC_AMD_VERSAL2_RPU' "$SOC_KCONFIG" \
            && grep -q 'cpu@0,clock-frequency' "$SOC_KCONFIG"; then
        # Scope the rewrite to the RPU block: substitute only between
        # `if SOC_AMD_VERSAL2_RPU` and its matching `endif`.
        sed -i '/^if SOC_AMD_VERSAL2_RPU/,/^endif # SOC_AMD_VERSAL2_RPU/ s|/cpus/cpu@0,clock-frequency|/cpus/cpu@2,clock-frequency|' \
            "$SOC_KCONFIG"
        echo "✓ Patched $SOC_KCONFIG: RPU block cpu@0 -> cpu@2 (lopper target)"
    elif grep -q '/cpus/cpu@2,clock-frequency' "$SOC_KCONFIG"; then
        echo "✓ SoC Kconfig already targets cpu@2 for the RPU"
    else
        echo "Warning: $SOC_KCONFIG layout changed; SYS_CLOCK_HW_CYCLES_PER_SEC patch may need updating"
    fi
fi

cd zephyr

# Check if test case exists
if [ ! -d "$TEST_CASE" ]; then
    echo "Error: Test case directory '$TEST_CASE' not found in zephyr/"
    echo "Available samples:"
    find samples -maxdepth 2 -type d -name "*" | head -20
    exit 1
fi

# Determine build flags — always pristine so DTS changes are picked up
BUILD_FLAGS="-b versal2_rpu -p always"

echo "⏳ Building project for versal2_rpu board (pristine build)..."

echo "   Compiling sources, linking, and generating ELF file..."
echo ""
west build $BUILD_FLAGS "$TEST_CASE" -- -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
echo ""
echo "✓ Build completed successfully"
echo ""

echo "========================================="
echo "Build completed successfully!"
echo "========================================="
echo "Output: zephyr/build/zephyr/zephyr.elf"
echo ""

# Step 13: Create deployment zip package driven by pack.json
echo "[13/13] Creating deployment zip package..."

TEST_NAME=$(basename "$TEST_CASE")
PACK_JSON="${TEST_CASE}/pack.json"
ZIP_DIR="${SCRIPT_DIR}/build_pkg_${TEST_NAME}"
ZIP_FILE="${SCRIPT_DIR}/${TEST_NAME}.zip"

if [ ! -f "$PACK_JSON" ]; then
    echo "  ✗ ${PACK_JSON} not found — skipping zip packaging"
else
    rm -rf "$ZIP_DIR"
    mkdir -p "$ZIP_DIR/${TEST_NAME}"

    # All paths in pack.json are relative to the test directory.
    # Python emits one line per entry: "file <path>" or "dir <path>"
    python3 -c "
import json, sys
with open(sys.argv[1]) as f:
    cfg = json.load(f)
for p in cfg.get('files', []):
    print('file\t' + p)
for p in cfg.get('dirs', []):
    print('dir\t' + p)
" "$PACK_JSON" | while IFS=$'\t' read -r kind src; do

        src_path="${TEST_CASE}/${src}"
        dest_name=$(basename "$src")

        if [ "$kind" = "file" ]; then
            if [ -f "$src_path" ]; then
                cp "$src_path" "$ZIP_DIR/${TEST_NAME}/${dest_name}"
                echo "  ✓ ${dest_name}"
            else
                echo "  ✗ ${dest_name} not found (${src_path})"
            fi
        elif [ "$kind" = "dir" ]; then
            if [ -d "$src_path" ]; then
                mkdir -p "$ZIP_DIR/${TEST_NAME}/${dest_name}"
                cp -r "$src_path"/* "$ZIP_DIR/${TEST_NAME}/${dest_name}/" 2>/dev/null || true
                echo "  ✓ ${dest_name}/"
            else
                echo "  ✗ ${dest_name}/ not found (${src_path})"
            fi
        fi
    done

    # Create zip
    rm -f "$ZIP_FILE"
    (cd "$ZIP_DIR" && zip -r "$ZIP_FILE" .)
    rm -rf "$ZIP_DIR"
    echo ""
    echo "✓ Deployment package created: ${ZIP_FILE}"
fi
echo ""

echo "========================================="
echo "Build ended at: $(date)"
echo "Log file: $LOG_FILE"
echo "========================================="
