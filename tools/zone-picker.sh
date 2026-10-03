#!/usr/bin/env bash
set -euo pipefail

# Launch the Cerberus Zone Picker: serve the static page over a local HTTP server
# and open it in a browser pre-pointed at a player's camera snapshot (the
# image-stream-server /image endpoint). Draw a box over each screen; copy the
# resulting "roi" block into /storage/sd/configs/config.json and restart the
# extension.
#
# Usage:
#   tools/zone-picker.sh --player <ip-or-host> [--port 20200] [--once]
#   tools/zone-picker.sh --url http://<player>:20200/image [--once]
#
# --port defaults to 20200 (the extension's image-stream-server port; set on the
# player via the networking.bs-image-stream-server-port registry key).
# --once disables the live refresh (use a single snapshot).
#
# Requires: python3 and curl.

PLAYER=""
PORT="20200"
URL=""
LIVE="1"

while [ $# -gt 0 ]; do
  case "$1" in
    --player) PLAYER="$2"; shift 2 ;;
    --port)   PORT="$2";   shift 2 ;;
    --url)    URL="$2";    shift 2 ;;
    --once)   LIVE="0";    shift ;;
    -h|--help)
      sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; exit 1 ;;
  esac
done

if [ -z "${URL}" ]; then
  [ -n "${PLAYER}" ] || { echo "ERROR: pass --player <ip> (or --url)" >&2; exit 1; }
  URL="http://${PLAYER}:${PORT}/image"
fi

command -v python3 >/dev/null 2>&1 || { echo "ERROR: python3 is required" >&2; exit 1; }
command -v curl >/dev/null 2>&1 || { echo "ERROR: curl is required" >&2; exit 1; }

echo "Checking camera snapshot at ${URL} ..."
if ! curl -fsS -m 5 -o /dev/null "${URL}"; then
  echo "WARNING: could not fetch ${URL} (is the player up and the image-stream-server running?)" >&2
  echo "         Continuing anyway -- you can also load a saved image file in the page." >&2
fi

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVE_DIR="$(mktemp -d)"
trap 'rm -rf "${SERVE_DIR}"' EXIT
cp "${SRC_DIR}/zone-picker.html" "${SRC_DIR}/zone-picker.logic.js" "${SERVE_DIR}/"

# Pick a free local port starting at 8781 (python exits 0 when the port is free).
port_free() {
  python3 -c "import socket,sys; s=socket.socket(); r=s.connect_ex(('127.0.0.1',int(sys.argv[1]))); s.close(); sys.exit(0 if r!=0 else 1)" "$1" >/dev/null 2>&1
}
LOCAL_PORT=8781
while ! port_free "${LOCAL_PORT}"; do
  LOCAL_PORT=$((LOCAL_PORT + 1))
  [ "${LOCAL_PORT}" -lt 8900 ] || { echo "ERROR: no free local port found" >&2; exit 1; }
done

ENC_URL="$(python3 -c "import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1], safe=''))" "${URL}")"
PAGE="http://127.0.0.1:${LOCAL_PORT}/zone-picker.html?url=${ENC_URL}&live=${LIVE}"

echo "Serving ${SERVE_DIR} on http://127.0.0.1:${LOCAL_PORT}"
echo "Opening ${PAGE}"
( cd "${SERVE_DIR}" && python3 -m http.server "${LOCAL_PORT}" --bind 127.0.0.1 >/dev/null 2>&1 ) &
SERVER_PID=$!
trap 'kill "${SERVER_PID}" 2>/dev/null || true; rm -rf "${SERVE_DIR}"' EXIT
sleep 0.5

if command -v xdg-open >/dev/null 2>&1; then xdg-open "${PAGE}" >/dev/null 2>&1 || true
elif command -v open >/dev/null 2>&1; then open "${PAGE}" || true
else echo "Open this URL in your browser: ${PAGE}"; fi

echo "Press Ctrl-C to stop the local server."
wait "${SERVER_PID}"
