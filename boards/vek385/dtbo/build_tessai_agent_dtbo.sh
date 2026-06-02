#!/usr/bin/env bash
# Build tessai-agent-add.dtbo from tessai-agent-add.dts.
#
# The overlay is resolved at apply time against the live FDT's /__symbols__;
# it does not patch the base DTB.  Boot with a symbol-exporting base DTB
# (see README.md).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DTS="${SCRIPT_DIR}/tessai-agent-add.dts"
DTBO="${SCRIPT_DIR}/tessai-agent-add.dtbo"

command -v dtc >/dev/null || { echo "error: dtc not in PATH" >&2; exit 1; }

echo "[build_tessai_agent_dtbo] ${DTS} -> ${DTBO}"
dtc -@ -I dts -O dtb -o "${DTBO}" "${DTS}"

echo "[build_tessai_agent_dtbo] wrote ${DTBO} ($(stat -c %s "${DTBO}") bytes)"
