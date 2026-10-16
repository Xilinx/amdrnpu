#!/bin/sh
# SPDX-License-Identifier: MIT
# Build the A78 device peer with the AMD EDF (Yocto) SDK produced by the vek385
# reference build:
#   .../vek385/rev-b/sw/yocto/build/tmp/deploy/sdk/amd-edf-glibc-*-toolchain-*.sh
#
# That SDK ships shared glibc only (no static libc.a), so the result is a
# DYNAMIC aarch64 ELF; glibc is provided by the A78 Yocto rootfs. For a fully
# static binary, point STATIC_CC at a toolchain that has libc.a (e.g. the Vitis
# GNU aarch64-linux-gnu-gcc) and pass STATIC=1.
#
# Overrides:
#   EDF_SDK_ENV=<path>  SDK environment-setup script (default below)
#   STATIC=1            link -static using $STATIC_CC (needs libc.a)
#   STATIC_CC=<gcc>     static toolchain (default aarch64-linux-gnu-gcc)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
RPMSG="$HERE/../../../lib/src/rpmsg-lite"
OUT="$HERE/echo_device"
SRCS="$HERE/main.c $HERE/mem_devmem.c $HERE/echo_device.c $HERE/../echo_config.c"
INC="-I$HERE -I$HERE/.. -I$RPMSG"
CFLAGS="-std=c11 -O2 -g -Wall -Wextra"

if [ "${STATIC:-0}" = "1" ]; then
	STATIC_CC="${STATIC_CC:-aarch64-linux-gnu-gcc}"
	echo "building STATIC with $STATIC_CC"
	set -x
	# shellcheck disable=SC2086
	"$STATIC_CC" $CFLAGS -static $INC $SRCS -o "$OUT"
	set +x
else
	# Installed copy of the vek385 build's SDK (same 26.06 release).
	SDK_ENV="${EDF_SDK_ENV:-/scratch/wendy/t20/sdk/environment-setup-cortexa72-cortexa53-amd-linux}"
	if [ -f "$SDK_ENV" ]; then
		echo "sourcing EDF SDK: $SDK_ENV"
		# shellcheck disable=SC1090
		. "$SDK_ENV"
	fi
	if [ -z "${CC:-}" ]; then
		echo "error: no SDK env sourced and CC unset." >&2
		echo "  source the EDF SDK first, or set EDF_SDK_ENV=<env-setup>" >&2
		exit 1
	fi
	echo "building with EDF SDK CC (dynamic)"
	set -x
	# shellcheck disable=SC2086
	$CC $CFLAGS $INC $SRCS -o "$OUT"
	set +x
fi

file "$OUT"
echo "built: $OUT"
