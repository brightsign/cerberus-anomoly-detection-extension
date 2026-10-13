#!/usr/bin/env bash
set -euo pipefail

# Cross-compile and install the anomaly_detection engine for each SoC against the
# SDK in the shared cache. Called by `make build-engine` (which first ensures the
# SDK, verifies the sysroot libs, and fetches the RKNN headers).
#
# Usage: scripts/build.sh [SOC ...]      (default: RK3588 RK3576 RK3568)
#   SoC names are case-insensitive; build dirs are build_<soc> (lowercase), which
#   is the name ./package reads. Install dirs are install/<SOC> (uppercase).

# shellcheck disable=SC1091
source "$(dirname "${BASH_SOURCE[0]}")/lib/cache.sh"   # sets SDK_ENV, CACHE_DIR, ...
REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

if [ ! -f "${SDK_ENV}" ]; then
    echo "ERROR: SDK env not found at ${SDK_ENV}" >&2
    echo "Run 'make fetch-sdk' (and see docs/PRE-REQS.md) to populate the cache." >&2
    exit 1
fi

# The engine's CMake builds the mosquitto broker from source and looks for the
# tarball in ./downloads before hitting mosquitto.org (often unreachable from a
# build box). Stage any tarballs the cache already holds so the build is offline.
if ls "${CACHE_DIR}"/downloads/*.tar.gz >/dev/null 2>&1; then
    mkdir -p "${REPO_ROOT}/downloads"
    cp -n "${CACHE_DIR}"/downloads/*.tar.gz "${REPO_ROOT}/downloads/" 2>/dev/null || true
    echo "Staged source tarballs from ${CACHE_DIR}/downloads into downloads/"
fi

# Source the SDK: brings in OECORE_TARGET_SYSROOT, CC/CXX, and the cross PATH.
# shellcheck disable=SC1090
source "${SDK_ENV}"

SOCS=("$@")
[ "${#SOCS[@]}" -gt 0 ] || SOCS=(RK3588 RK3576 RK3568)

for soc in "${SOCS[@]}"; do
    socl="$(echo "${soc}" | tr '[:upper:]' '[:lower:]')"
    bdir="${REPO_ROOT}/build_${socl}"
    echo "== building ${soc} in build_${socl} =="
    mkdir -p "${bdir}"
    cmake -S "${REPO_ROOT}" -B "${bdir}" \
        -DOECORE_TARGET_SYSROOT="${OECORE_TARGET_SYSROOT}" \
        -DTARGET_SOC="${socl}" \
        -DCMAKE_BUILD_TYPE=Release
    make -C "${bdir}" -j"$(nproc)"
    make -C "${bdir}" install
done

echo "build.sh: done (${SOCS[*]})"
