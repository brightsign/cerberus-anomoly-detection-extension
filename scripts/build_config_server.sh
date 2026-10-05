#!/bin/bash
set -euo pipefail

# Cross-compile the in-repo config-server (pure Go stdlib, no cgo) for the player
# (aarch64) and write the binary to the requested output path. No BrightSign SDK
# is needed -- Go cross-compiles on its own.
#
# Usage: build_config_server.sh <src-dir> <out-binary>

SRC_DIR="$1"
OUT_BIN="$2"

GO_BIN="$(command -v go || true)"
if [ -z "${GO_BIN}" ]; then
  for p in /usr/local/go/bin/go /usr/lib/go/bin/go "${HOME}/go/bin/go"; do
    [ -x "$p" ] && GO_BIN="$p" && break
  done
fi
[ -n "${GO_BIN}" ] || { echo "ERROR: go toolchain not found on PATH" >&2; exit 1; }

echo "Building config-server (linux/arm64) with $("${GO_BIN}" version)"
( cd "${SRC_DIR}" && GOOS=linux GOARCH=arm64 CGO_ENABLED=0 GOTOOLCHAIN=local \
    "${GO_BIN}" build -trimpath -ldflags "-s -w" -o "${OUT_BIN}" . )
echo "config-server -> ${OUT_BIN}"
