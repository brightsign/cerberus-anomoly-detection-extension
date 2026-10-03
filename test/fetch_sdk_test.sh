#!/usr/bin/env bash
# Host test for scripts/fetch-sdk.sh guard rails (no real SDK install).
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SH="$REPO/scripts/fetch-sdk.sh"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0

# Empty cache, no installer on hand -> exit 1 and name brightsign-sdk-builder.
# Run from an empty CWD with REPO_ROOT pointed at an empty dir so the installer
# search (CWD / repo root / cache dir) finds nothing -- isolates the test from
# any brightsign-x86_64-cobra-toolchain-*.sh still present in the real tree.
mkdir -p "$TMP/empty"
out="$(cd "$TMP/empty" && ARGUS_CACHE_DIR="$TMP/empty" REPO_ROOT="$TMP/empty" bash "$SH" 2>&1)"; rc=$?
{ [ "$rc" = 1 ] && echo "$out" | grep -q "brightsign-sdk-builder"; } \
  && echo "  ok   empty-cache guidance" || { echo "  FAIL empty-cache: rc=$rc out=$out"; fail=1; }

# A cache/sdk that exists but whose env-setup does NOT bake this path -> error, exit 1.
mkdir -p "$TMP/stale/sdk/sysroots/aarch64-oe-linux"
echo "SDKTARGETSYSROOT=/some/other/path/sysroots/aarch64-oe-linux" > "$TMP/stale/sdk/environment-setup-aarch64-oe-linux"
out="$(ARGUS_CACHE_DIR="$TMP/stale" bash "$SH" 2>&1)"; rc=$?
{ [ "$rc" = 1 ] && echo "$out" | grep -qi "not .*self-consistent\|does not reference"; } \
  && echo "  ok   stale-sdk rejected" || { echo "  FAIL stale-sdk: rc=$rc out=$out"; fail=1; }

# A self-consistent cache/sdk -> exit 0.
mkdir -p "$TMP/good/sdk/sysroots/aarch64-oe-linux"
echo "SDKTARGETSYSROOT=$TMP/good/sdk/sysroots/aarch64-oe-linux" > "$TMP/good/sdk/environment-setup-aarch64-oe-linux"
out="$(ARGUS_CACHE_DIR="$TMP/good" bash "$SH" 2>&1)"; rc=$?
[ "$rc" = 0 ] && echo "  ok   good-sdk accepted" || { echo "  FAIL good-sdk: rc=$rc out=$out"; fail=1; }

[ "$fail" = 0 ] && echo "ALL PASS" || { echo "FAILURES"; exit 1; }
