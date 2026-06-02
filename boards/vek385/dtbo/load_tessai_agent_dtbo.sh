#!/bin/sh
# Apply the tessai-agent device-tree overlay to the live kernel and
# (optionally) load the tessai_agent driver module.
#
# Adds a /tessai-agent node (compatible "amd,tessai-agent") with:
#   - amd,remoteproc -> remoteproc r52f@1  (label r5f_1 in base DTB).
#   - bank@0 memory-region -> tess_rpu_gem_carveout (RPU GEM, 0x20000000..0x3fffffff)
#   - bank@1 memory-region -> app_bank0 (high DDR CMA pool)
#
# REQUIRES the kernel to be booted with a multi-DTB / symbol-exporting base
# DTB that defines r5f_1, tess_rpu_gem_carveout, and app_bank0 in
# /__symbols__ (same as amdxdna VEK385 prep).  See README.md.
#
# Usage:
#   ./load_tessai_agent_dtbo.sh [apply|remove|status]   (default: apply)
#
# Env overrides:
#   DTBO=/path/to/tessai-agent-add.dtbo
#   OVERLAY_NAME=tessai-agent          (configfs subdir name)
#   LOAD_MODULE=0                      (set to 1 to modprobe tessai_agent after apply)

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DTBO="${DTBO:-${SCRIPT_DIR}/tessai-agent-add.dtbo}"
OVERLAY_NAME="${OVERLAY_NAME:-tessai-agent}"
LOAD_MODULE="${LOAD_MODULE:-0}"

CONFIGFS=/sys/kernel/config
OVL_ROOT="${CONFIGFS}/device-tree/overlays"
OVL_DIR="${OVL_ROOT}/${OVERLAY_NAME}"
DT_NODE=/proc/device-tree/tessai-agent

action="${1:-apply}"

log()  { printf '[load_tessai_agent_dtbo] %s\n' "$*"; }
die()  { printf '[load_tessai_agent_dtbo] ERROR: %s\n' "$*" >&2; exit 1; }

ensure_configfs() {
	if ! mountpoint -q "${CONFIGFS}"; then
		log "mounting configfs at ${CONFIGFS}"
		mkdir -p "${CONFIGFS}"
		mount -t configfs none "${CONFIGFS}"
	fi
	[ -d "${OVL_ROOT}" ] || \
		die "kernel has no ${OVL_ROOT} - CONFIG_OF_CONFIGFS missing?"
}

case "${action}" in
apply)
	[ -r "${DTBO}" ] || die "overlay not found: ${DTBO}"
	ensure_configfs

	if [ -d "${OVL_DIR}" ]; then
		status="$(cat "${OVL_DIR}/status" 2>/dev/null || echo unknown)"
		log "overlay '${OVERLAY_NAME}' already loaded (status=${status}); nothing to do"
	else
		log "applying ${DTBO} as overlay '${OVERLAY_NAME}'"
		mkdir "${OVL_DIR}"
		if ! dd if="${DTBO}" of="${OVL_DIR}/dtbo" bs=4k status=none; then
			rmdir "${OVL_DIR}" 2>/dev/null || true
			die "failed to write dtbo (kernel rejected the overlay)"
		fi
		status="$(cat "${OVL_DIR}/status" 2>/dev/null || echo unknown)"
		log "status=${status}"
		[ "${status}" = "applied" ] || die "overlay status is '${status}', expected 'applied'"
	fi

	if [ -d "${DT_NODE}" ]; then
		log "${DT_NODE} present:"
		ls "${DT_NODE}" | sed 's/^/    /'
	else
		die "${DT_NODE} missing after apply"
	fi

	if [ "${LOAD_MODULE}" = "1" ]; then
		log "modprobe tessai_agent"
		modprobe tessai_agent || die "modprobe tessai_agent failed"
		dmesg | tail -30 | sed 's/^/    /'
	fi
	;;

remove)
	if [ "${LOAD_MODULE}" = "1" ]; then
		log "rmmod tessai_agent (best-effort)"
		modprobe -r tessai_agent 2>/dev/null || true
	fi
	if [ -d "${OVL_DIR}" ]; then
		log "removing overlay '${OVERLAY_NAME}'"
		rmdir "${OVL_DIR}"
	else
		log "overlay '${OVERLAY_NAME}' not loaded"
	fi
	;;

status)
	if [ -d "${OVL_DIR}" ]; then
		log "overlay '${OVERLAY_NAME}' status: $(cat "${OVL_DIR}/status" 2>/dev/null || echo unknown)"
	else
		log "overlay '${OVERLAY_NAME}' not loaded"
	fi
	if [ -d "${DT_NODE}" ]; then
		log "${DT_NODE} properties:"
		for f in "${DT_NODE}"/*; do
			printf '    %s = ' "${f##*/}"
			case "${f##*/}" in
			compatible|name|'#'*)
				tr -d '\0' < "$f" ; printf '\n' ;;
			*)
				od -An -tx4 -N16 "$f" 2>/dev/null | tr -s ' ' || true
				printf '\n' ;;
			esac
		done
		if [ -d "${DT_NODE}/bank@0" ]; then
			log "  bank@0:"
			ls "${DT_NODE}/bank@0" 2>/dev/null | sed 's/^/      /' || true
		fi
		if [ -d "${DT_NODE}/bank@1" ]; then
			log "  bank@1:"
			ls "${DT_NODE}/bank@1" 2>/dev/null | sed 's/^/      /' || true
		fi
	else
		log "${DT_NODE} not present"
	fi
	;;

*)
	die "unknown action '${action}' (expected: apply | remove | status)"
	;;
esac
