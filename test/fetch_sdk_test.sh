#!/usr/bin/env bash
# Host test for scripts/fetch-sdk.sh guard rails (no real SDK install, no network).
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SH="$REPO/scripts/fetch-sdk.sh"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0

# Empty cache, no installer, auto-build DISABLED -> exit 1 and name the builder.
# Run from an empty CWD with REPO_ROOT pointed at an empty dir so the installer
# search (CWD / repo root / cache dir) finds nothing. BS_SDK_AUTOBUILD=0 keeps the
# test hermetic -- the auto-build path is exercised separately below with a stub.
mkdir -p "$TMP/empty"
out="$(cd "$TMP/empty" && BS_SDK_AUTOBUILD=0 BS_CACHE_DIR="$TMP/empty" REPO_ROOT="$TMP/empty" bash "$SH" 2>&1)"; rc=$?
{ [ "$rc" = 1 ] && echo "$out" | grep -q "brightsign-sdk-builder"; } \
  && echo "  ok   empty-cache guidance (autobuild off)" || { echo "  FAIL empty-cache: rc=$rc out=$out"; fail=1; }

# A cache/sdk that exists but whose env-setup does NOT bake this path -> error, exit 1.
mkdir -p "$TMP/stale/sdk/sysroots/aarch64-oe-linux"
echo "SDKTARGETSYSROOT=/some/other/path/sysroots/aarch64-oe-linux" > "$TMP/stale/sdk/environment-setup-aarch64-oe-linux"
out="$(BS_CACHE_DIR="$TMP/stale" bash "$SH" 2>&1)"; rc=$?
{ [ "$rc" = 1 ] && echo "$out" | grep -qi "not .*self-consistent\|does not reference"; } \
  && echo "  ok   stale-sdk rejected" || { echo "  FAIL stale-sdk: rc=$rc out=$out"; fail=1; }

# A self-consistent cache/sdk -> exit 0.
mkdir -p "$TMP/good/sdk/sysroots/aarch64-oe-linux"
echo "SDKTARGETSYSROOT=$TMP/good/sdk/sysroots/aarch64-oe-linux" > "$TMP/good/sdk/environment-setup-aarch64-oe-linux"
out="$(BS_CACHE_DIR="$TMP/good" bash "$SH" 2>&1)"; rc=$?
[ "$rc" = 0 ] && echo "  ok   good-sdk accepted" || { echo "  FAIL good-sdk: rc=$rc out=$out"; fail=1; }

# Auto-build path: with a locatable builder (SDK_BUILDER_DIR), fetch-sdk invokes
# `make -C <builder> sdk ARGUS_CACHE_DIR=<cache>` and converges to a
# self-consistent SDK. The stub builder writes a baked env-setup + a librknnrt
# stub (so fetch-sdk's runtime-lib fetch is skipped -- keeps the test offline).
mkdir -p "$TMP/builder"
cat > "$TMP/builder/Makefile" <<'MK'
.RECIPEPREFIX = >
sdk:
> mkdir -p "$(ARGUS_CACHE_DIR)/sdk/sysroots/aarch64-oe-linux/usr/lib"
> printf 'SDKTARGETSYSROOT=%s/sdk/sysroots/aarch64-oe-linux\n' "$(ARGUS_CACHE_DIR)" > "$(ARGUS_CACHE_DIR)/sdk/environment-setup-aarch64-oe-linux"
> : > "$(ARGUS_CACHE_DIR)/sdk/sysroots/aarch64-oe-linux/usr/lib/librknnrt.so"
MK
out="$(SDK_BUILDER_DIR="$TMP/builder" BS_CACHE_DIR="$TMP/autobuild" bash "$SH" 2>&1)"; rc=$?
{ [ "$rc" = 0 ] && grep -q "SDKTARGETSYSROOT=$TMP/autobuild/sdk/sysroots" "$TMP/autobuild/sdk/environment-setup-aarch64-oe-linux" 2>/dev/null; } \
  && echo "  ok   auto-build via builder" || { echo "  FAIL auto-build: rc=$rc out=$out"; fail=1; }

[ "$fail" = 0 ] && echo "ALL PASS" || { echo "FAILURES"; exit 1; }
