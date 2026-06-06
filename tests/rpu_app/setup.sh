#!/bin/bash

# One-time system setup for Zephyr AMD RPU build environment
# Run this script ONCE on a new build machine / runner VDI before using build.sh
#
# Usage: sudo ./setup.sh
#    or: ./setup.sh  (will prompt for sudo password)

set -e

echo "========================================="
echo "RPURuntime - One-Time System Setup"
echo "========================================="
echo ""
echo "This script installs system packages required by build.sh."
echo "Run it once on a new machine. After that, only build.sh is needed."
echo ""

REQUIRED_PACKAGES="git cmake ninja-build gperf ccache dfu-util device-tree-compiler wget python3-dev python3-pip python3-setuptools python3-tk python3-wheel xz-utils file make gcc gcc-multilib g++-multilib libsdl2-dev libmagic1 python3-venv"

# Check which packages are missing
MISSING_PACKAGES=""
for pkg in $REQUIRED_PACKAGES; do
    if ! dpkg -l 2>/dev/null | grep -q "^ii  $pkg"; then
        MISSING_PACKAGES="$MISSING_PACKAGES $pkg"
    fi
done

if [ -z "$MISSING_PACKAGES" ]; then
    echo "All required packages are already installed."
    echo ""
    echo "Installed packages:"
    for pkg in $REQUIRED_PACKAGES; do
        echo "  ✓ $pkg"
    done
    echo ""
    echo "You can run build.sh directly."
    exit 0
fi

echo "Missing packages:"
echo "$MISSING_PACKAGES" | tr ' ' '\n' | sed 's/^/  - /'
echo ""
echo "Installing..."
echo ""

SUDO_CMD="sudo"
ALTERNATIVE_SUDO="/tool/aecg-sudo/sudo/bin/sudo"

if sudo apt install --no-install-recommends -y $REQUIRED_PACKAGES 2>/dev/null; then
    echo ""
    echo "✓ Dependencies installed successfully"
elif [ -f "$ALTERNATIVE_SUDO" ]; then
    echo "Default sudo failed, trying alternative: $ALTERNATIVE_SUDO"
    if $ALTERNATIVE_SUDO apt install --no-install-recommends -y $REQUIRED_PACKAGES; then
        echo ""
        echo "✓ Dependencies installed successfully using alternative sudo"
    else
        echo ""
        echo "ERROR: Failed to install packages with both sudo methods."
        echo "Please install manually:"
        echo ""
        echo "  sudo apt install --no-install-recommends $REQUIRED_PACKAGES"
        exit 1
    fi
else
    echo ""
    echo "ERROR: sudo failed and alternative sudo not found at $ALTERNATIVE_SUDO"
    echo "Please install manually:"
    echo ""
    echo "  sudo apt install --no-install-recommends $REQUIRED_PACKAGES"
    exit 1
fi

echo ""
echo "========================================="
echo "Verifying installation:"
echo "  CMake version: $(cmake --version | head -n1)"
echo "  Python3 version: $(python3 --version)"
echo "  DTC version: $(dtc --version 2>&1 | head -n1)"
echo "========================================="
echo ""
echo "Setup complete. You can now run: ./build.sh tests/tessai_agent_test"
echo "========================================="
