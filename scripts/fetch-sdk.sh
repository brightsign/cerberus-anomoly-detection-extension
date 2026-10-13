#!/usr/bin/env bash
set -euo pipefail

# Ensures the custom BrightSign aarch64 cross-compile SDK is available in the
# shared build cache (CACHE_DIR/sdk -- see scripts/lib/cache.sh). The SDK is
# provisioned once per build box by the `brightsign-sdk-builder` repo, which
# installs it straight into the cache; this script detects it there. As a
# convenience it will also install from a toolchain installer you have on hand.
#
# "Self-contained path" matters: OE SDK installers bake absolute paths
# (SDKTARGETSYSROOT, PATH, CC/CXX --sysroot=..., and the ELF INTERP/RPATH of the
# cross-toolchain binaries themselves) into the tree at install time, relocated to
# wherever `-d <dir>` points. Because every project resolves the SAME absolute
# CACHE_DIR/sdk path, one correct install serves them all; copying in an SDK
# relocated for a different path silently breaks the build.

# shellcheck disable=SC1091
source "$(dirname "${BASH_SOURCE[0]}")/lib/cache.sh"   # sets SDK_DIR, SDK_ENV, CACHE_DIR
REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
ENV_SCRIPT="${SDK_ENV}"
INSTALLER_GLOB="brightsign-x86_64-cobra-toolchain-*.sh"

# An SDK is usable only if present AND baked for THIS cache's absolute path. A
# stale-path SDK fails with "cannot execute: required file not found" when the
# cross compiler is invoked, since its ELF INTERP points elsewhere.
sdk_is_self_consistent() {
    [ -d "${SDK_DIR}/sysroots/aarch64-oe-linux" ] || return 1
    [ -f "${ENV_SCRIPT}" ] || return 1
    grep -q "SDKTARGETSYSROOT=${SDK_DIR}/sysroots/aarch64-oe-linux" "${ENV_SCRIPT}"
}

if sdk_is_self_consistent; then
    echo "SDK already installed and self-consistent at ${SDK_DIR}"
    exit 0
fi

if [ -e "${SDK_DIR}" ]; then
    echo "ERROR: ${SDK_DIR} exists but is not a self-consistent SDK for this cache" >&2
    echo "(its environment-setup-aarch64-oe-linux does not reference ${SDK_DIR})." >&2
    echo "Remove it and re-run to install fresh: rm -rf ${SDK_DIR}" >&2
    exit 1
fi

# Install from a toolchain installer if one is on hand: $1, else the shared cache
# dir, else the repo root, else CWD.
INSTALLER="${1:-}"
if [ -z "${INSTALLER}" ]; then
    # shellcheck disable=SC2086
    INSTALLER="$(ls ${CACHE_DIR}/${INSTALLER_GLOB} 2>/dev/null | head -n1 || true)"
fi
if [ -z "${INSTALLER}" ]; then
    # shellcheck disable=SC2086
    INSTALLER="$(ls ${REPO_ROOT}/${INSTALLER_GLOB} 2>/dev/null | head -n1 || true)"
fi
if [ -z "${INSTALLER}" ]; then
    # shellcheck disable=SC2086
    INSTALLER="$(ls ./${INSTALLER_GLOB} 2>/dev/null | head -n1 || true)"
fi

# Printed when the SDK is absent and we cannot (or are told not to) auto-build it.
print_sdk_guidance() {
    cat <<EOF
No cross-compile SDK found in the shared cache, and no toolchain installer on hand.

The SDK is built ONCE per build box by the brightsign-sdk-builder repo
(https://github.com/brightsign/brightsign-sdk-builder): it downloads the
BrightSign OS source and builds the SDK + RKNN toolkit + rknn_tk2 image into the
shared cache this repo reads. The cache is shared, so many of our example
extensions reuse it.

This build normally auto-builds the SDK for you; it did not here (auto-build
disabled, or git/docker/network unavailable, or the builder could not be
located). To do it by hand:

    git clone https://github.com/brightsign/brightsign-sdk-builder.git
    make -C brightsign-sdk-builder sdk ARGUS_CACHE_DIR=${CACHE_DIR}

That populates ${SDK_DIR}. Then re-run this build.

Alternatively, if you already have a brightsign-x86_64-cobra-toolchain-*.sh
installer, install from it directly:
    - pass its path:  make fetch-sdk SDK_INSTALLER=/path/to/installer.sh
    - or drop it in the shared cache dir (see 'make cache-info') or the repo root.

Do NOT symlink or copy in an already-extracted sdk/ directory relocated for a
different path -- its baked paths will fail. Always install with -d pointed at the
shared cache path so OE relocation bakes the right path in.
EOF
}

# Find a brightsign-sdk-builder checkout to build the SDK with; clone one next to
# the cache if none is present. Honors SDK_BUILDER_DIR, then known siblings, then
# GitHub. Prints the resolved path on stdout; all chatter goes to stderr.
locate_sdk_builder() {
    if [ -n "${SDK_BUILDER_DIR:-}" ]; then
        if [ -f "${SDK_BUILDER_DIR}/Makefile" ]; then (cd "${SDK_BUILDER_DIR}" && pwd); return 0; fi
        echo "SDK_BUILDER_DIR=${SDK_BUILDER_DIR} has no Makefile" >&2; return 1
    fi
    local d
    for d in \
        "${REPO_ROOT}/../argus-all/brightsign-sdk-builder" \
        "${REPO_ROOT}/../brightsign-sdk-builder" \
        "${CACHE_DIR}/../brightsign-sdk-builder"; do
        if [ -f "${d}/Makefile" ]; then (cd "${d}" && pwd); return 0; fi
    done
    local clone_dir="${CACHE_DIR%/}/../brightsign-sdk-builder"
    echo "brightsign-sdk-builder not found locally; cloning into ${clone_dir}..." >&2
    if command -v git >/dev/null 2>&1 && \
       git clone --depth 1 https://github.com/brightsign/brightsign-sdk-builder.git "${clone_dir}" >&2; then
        (cd "${clone_dir}" && pwd); return 0
    fi
    return 1
}

# Build just the SDK into THIS cache. brightsign-sdk-builder selects its output
# with ARGUS_CACHE_DIR, so pass our absolute CACHE_DIR; it installs into SDK_DIR.
auto_build_sdk() {
    local builder
    builder="$(locate_sdk_builder)" || return 1
    echo "==> No SDK in the cache and no installer on hand." >&2
    echo "==> Auto-building the cross SDK into the shared cache." >&2
    echo "    builder : ${builder}" >&2
    echo "    cache   : ${CACHE_DIR}" >&2
    echo "    This needs Docker + network + ~25GB and takes HOURS." >&2
    echo "    (Set BS_SDK_AUTOBUILD=0 to disable and get manual instructions instead.)" >&2
    make -C "${builder}" sdk ARGUS_CACHE_DIR="${CACHE_DIR}" 1>&2
}

if [ -z "${INSTALLER}" ] || [ ! -f "${INSTALLER}" ]; then
    # No installer: auto-build the SDK into the (configured-but-empty) cache,
    # unless disabled. This is what makes `make build` provision the SDK on a
    # fresh box without a separate manual step.
    if [ "${BS_SDK_AUTOBUILD:-1}" = "1" ]; then
        auto_build_sdk || { print_sdk_guidance; exit 1; }
    else
        print_sdk_guidance
        exit 1
    fi
else
    echo "Installing SDK from ${INSTALLER} into ${SDK_DIR} (self-relocating)..."
    chmod +x "${INSTALLER}"
    # -y: non-interactive. -d: install (and relocate all baked absolute paths,
    # including the cross-toolchain binaries' ELF INTERP/RPATH) into SDK_DIR.
    "${INSTALLER}" -y -d "${SDK_DIR}"
fi

if ! sdk_is_self_consistent; then
    echo "ERROR: SDK present but environment-setup-aarch64-oe-linux does not" >&2
    echo "reference ${SDK_DIR} -- provisioning did not take effect as expected." >&2
    exit 1
fi

# The RKNN runtime library ships separately from the BrightSign SDK installer.
# Pinned to the same RKNN version as prep.sh and model compilation so the
# runtime, API headers, and compiled models stay compatible.
RKNN_TAG="${RKNN_TAG:-v2.3.0}"
RKNN_LIB="${SDK_DIR}/sysroots/aarch64-oe-linux/usr/lib/librknnrt.so"
if [ ! -f "${RKNN_LIB}" ]; then
    echo "Fetching librknnrt.so (Rockchip NPU runtime, ${RKNN_TAG})..."
    wget -O "${RKNN_LIB}" \
        "https://github.com/airockchip/rknn-toolkit2/raw/${RKNN_TAG}/rknpu2/runtime/Linux/librknn_api/aarch64/librknnrt.so"
fi

echo "SDK installed and self-consistent at ${SDK_DIR}"
echo "Source it before building: source ${SDK_DIR}/environment-setup-aarch64-oe-linux"
echo "Or just run: make build-engine"
