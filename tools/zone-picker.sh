#!/usr/bin/env bash
set -euo pipefail

# Open the on-player Zone Picker. The picker is served by the config-server that
# runs on the player (started by bsext_init); it reads and writes the live config
# at /storage/sd/configs/config.json, with Save and "Save & Restart" buttons.
#
# Usage:
#   tools/zone-picker.sh --player <ip-or-host> [--port 20300]
#
# --port defaults to 20300 (registry key networking.bs-config-server-port).
#
# Offline editing without a running player: open
# srv/config-server/web/zone-picker.html directly in a browser and use the
# "load config.json" / "Download config.json" buttons (file-drop workflow).

PLAYER=""
PORT="20300"

while [ $# -gt 0 ]; do
  case "$1" in
    --player) PLAYER="$2"; shift 2 ;;
    --port)   PORT="$2";   shift 2 ;;
    -h|--help) sed -n '3,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; exit 1 ;;
  esac
done

[ -n "${PLAYER}" ] || { echo "ERROR: --player <ip-or-host> required" >&2; exit 1; }

URL="http://${PLAYER}:${PORT}/"
echo "Opening on-player zone picker: ${URL}"
if command -v xdg-open >/dev/null 2>&1; then xdg-open "${URL}" >/dev/null 2>&1 || true
elif command -v open >/dev/null 2>&1; then open "${URL}" || true
else echo "Open this URL in your browser: ${URL}"; fi
