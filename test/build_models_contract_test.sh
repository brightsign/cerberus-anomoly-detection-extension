#!/usr/bin/env bash
# Host test for scripts/build-models.sh: a SOC whose outputs already exist must
# be skipped WITHOUT invoking docker (idempotency + the filename contract).
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SH="$REPO/scripts/build-models.sh"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0

# Pre-populate the expected outputs for RK3588 so the script must SKIP without
# needing Docker. Filenames are the contract bsext_init/app depend on.
mkdir -p "$TMP/RK3588"
: > "$TMP/RK3588/mobilenetv2-embedding-rk3588.rknn"
: > "$TMP/RK3588/yolox_s_rk3588.rknn"
: > "$TMP/RK3588/mobilenetv2_config.json"

# DOCKER points at /bin/false: if the script tries docker on a complete SOC, it fails.
out="$(DOCKER=/bin/false ARGUS_CACHE_DIR="$TMP/cache" bash "$SH" "$TMP" RK3588 2>&1)"; rc=$?
{ [ "$rc" = 0 ] && echo "$out" | grep -qi "skip\|already"; } \
  && echo "  ok   complete SOC skipped (no docker)" || { echo "  FAIL idempotency rc=$rc out=$out"; fail=1; }

[ "$fail" = 0 ] && echo "ALL PASS" || { echo "FAILURES"; exit 1; }
