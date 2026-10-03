#!/usr/bin/env bash
# Host test for scripts/lib/cache.sh path resolution.
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIB="$REPO/scripts/lib/cache.sh"
fail=0
check() { # label actual expected
  if [ "$2" = "$3" ]; then echo "  ok   $1"; else echo "  FAIL $1: got '$2' want '$3'"; fail=1; fi
}

# Default: parallel to repo, normalized absolute. Unset ARGUS_CACHE_DIR so the
# test is hermetic even when the caller (e.g. `make`) exported an auto-discovered
# value -- cache.sh's default is what is under test here.
exp_default="$(realpath -m "$REPO/../argus-build-cache")"
check "default cache"   "$(env -u ARGUS_CACHE_DIR bash "$LIB" print cache)"   "$exp_default"
check "default sdk"     "$(env -u ARGUS_CACHE_DIR bash "$LIB" print sdk)"      "$exp_default/sdk"
check "default sdk-env" "$(env -u ARGUS_CACHE_DIR bash "$LIB" print sdk-env)"  "$exp_default/sdk/environment-setup-aarch64-oe-linux"
check "default toolkit" "$(env -u ARGUS_CACHE_DIR bash "$LIB" print toolkit)"  "$exp_default/toolkit"
check "default models"  "$(env -u ARGUS_CACHE_DIR bash "$LIB" print models)"   "$exp_default/models"

# Absolute override is used verbatim.
check "abs override"    "$(ARGUS_CACHE_DIR=/tmp/xyzcache bash "$LIB" print cache)" "/tmp/xyzcache"

# Relative override resolves against REPO_ROOT and normalizes '..'.
check "rel override"    "$(ARGUS_CACHE_DIR=sub/../bc bash "$LIB" print cache)" "$(realpath -m "$REPO/bc")"

[ "$fail" = 0 ] && echo "ALL PASS" || { echo "FAILURES"; exit 1; }
