#!/usr/bin/env bash
set -euo pipefail

# Verify the SDK sysroot ships the native libraries the anomaly_detection engine
# links from OECORE_TARGET_SYSROOT. Unlike the voice-llm reference (which builds
# small audio libs into the sysroot), this engine's heavy dependencies -- OpenCV,
# GStreamer, GLib, RGA, TurboJPEG, the mosquitto client -- belong in the OE SDK
# itself and are provisioned by the brightsign-sdk-builder repo. Cross-building
# OpenCV here would be wrong; so this script GATES: it checks the sysroot and, if
# anything required is missing, fails with an actionable message rather than
# letting cmake fail deep in a link step.
#
# It builds nothing by default. FORCE is reserved for future per-lib additions.

# shellcheck disable=SC1091
source "$(dirname "${BASH_SOURCE[0]}")/lib/cache.sh"   # sets SDK_ENV, SDK_DIR, ...

if [ ! -f "${SDK_ENV}" ]; then
    echo "ERROR: SDK env not found at ${SDK_ENV} -- run 'make fetch-sdk' first." >&2
    exit 1
fi
# shellcheck disable=SC1090
source "${SDK_ENV}"   # OECORE_TARGET_SYSROOT

SYSROOT="${OECORE_TARGET_SYSROOT}"
INCDIR="${SYSROOT}/usr/include"
LIBDIR="${SYSROOT}/usr/lib"

# A dependency is satisfied if a representative header AND a representative lib
# are present. (Empty header field = lib-only check, e.g. RGA.)
missing=()
require() { # label  header-relpath(optional)  lib-glob
    local label="$1" hdr="$2" libglob="$3"
    local ok=1
    if [ -n "${hdr}" ] && [ ! -e "${INCDIR}/${hdr}" ]; then ok=0; fi
    # shellcheck disable=SC2086
    if ! ls ${LIBDIR}/${libglob} >/dev/null 2>&1; then ok=0; fi
    if [ "${ok}" = 1 ]; then
        echo "  ok   ${label}"
    else
        echo "  MISSING ${label}  (need ${hdr:+${INCDIR}/${hdr} + }${LIBDIR}/${libglob})"
        missing+=("${label}")
    fi
}

echo "Checking SDK sysroot for the engine's native libraries:"
echo "  sysroot: ${SYSROOT}"
require "OpenCV"          "opencv4/opencv2/core.hpp"      "libopencv_core.so*"
require "GStreamer"       "gstreamer-1.0/gst/gst.h"       "libgstreamer-1.0.so*"
require "GStreamer app"   "gstreamer-1.0/gst/app/gstappsink.h" "libgstapp-1.0.so*"
require "GLib"            "glib-2.0/glib.h"               "libglib-2.0.so*"
require "GObject"         ""                              "libgobject-2.0.so*"
require "RGA"             ""                              "librga.so*"
require "TurboJPEG"       "turbojpeg.h"                   "libturbojpeg.so*"
require "mosquitto client" "mosquitto.h"                  "libmosquitto.so*"

# librknnrt is Rockchip-proprietary and is placed by scripts/fetch-sdk.sh (and
# also linked from include/ by scripts/prep.sh); warn rather than fail here.
if ! ls "${LIBDIR}"/librknnrt.so* >/dev/null 2>&1; then
    echo "  note  librknnrt.so not in sysroot yet -- fetch-sdk/prep supply it; continuing"
fi

if [ "${#missing[@]}" -ne 0 ]; then
    cat >&2 <<EOF

ERROR: the SDK sysroot is missing libraries the engine links: ${missing[*]}

These belong in the BrightSign cross SDK, which is built by the
brightsign-sdk-builder repo (https://github.com/brightsign/brightsign-sdk-builder)
-- they are NOT cross-built per-repo here (building OpenCV/GStreamer by hand is
the wrong layer).

Fix: rebuild/repopulate the shared cache so the SDK sysroot includes the missing
packages, then re-run. See docs/PRE-REQS.md (Phase 1). Cache in use:
  ${SDK_DIR}
EOF
    exit 1
fi

echo "All required SDK sysroot libraries are present."
