#!/bin/sh
# capture_tv_off_prototype.sh
#
# Runs ON the BrightSign device inside the mounted extension.
# Captures TV_OFF embedding prototypes, then restarts in normal mode.
#
# Usage (on device, after SSH in):
#   /var/volatile/bsext/ext_npu_anomaly/capture_tv_off_prototype.sh [MAX_TVS]
#
# Examples:
#   ./capture_tv_off_prototype.sh       # prompts for TV count
#   ./capture_tv_off_prototype.sh 1
#   ./capture_tv_off_prototype.sh 2

BSEXT_DIR="$(dirname "$(realpath "$0")")"
BSEXT="${BSEXT_DIR}/bsext_init"
# If run from /storage/sd/ (not from extension dir), find bsext_init at install path
if [ ! -x "$BSEXT" ]; then
    BSEXT="/var/volatile/bsext/ext_npu_anomaly/bsext_init"
fi
if [ ! -x "$BSEXT" ]; then
    echo "ERROR: bsext_init not found. Run this script from the extension directory or /storage/sd/." >&2
    exit 1
fi
CONFIG="/storage/sd/configs/config.json"
PROTO_OUTPUT="/storage/sd/osd_prototypes.json"
CAPTURE_SECONDS=40

MAX_TVS="${1:-}"

# ── Input ─────────────────────────────────────────────────────────────────────

if [ -z "$MAX_TVS" ]; then
    printf "How many TVs to capture prototypes for? (1-5): "
    read -r MAX_TVS
fi

if ! echo "$MAX_TVS" | grep -qE '^[1-5]$'; then
    echo "ERROR: max_tvs must be a number between 1 and 5" >&2
    exit 1
fi

if [ ! -f "$CONFIG" ]; then
    echo "ERROR: Config not found at ${CONFIG}" >&2
    echo "       Create /storage/sd/configs/config.json first." >&2
    exit 1
fi

# ── Summary ───────────────────────────────────────────────────────────────────

echo ""
echo "========================================="
echo "  TV_OFF Prototype Capture"
echo "========================================="
echo "  TVs      : ${MAX_TVS}"
echo "  Config   : ${CONFIG}"
echo "  Output   : ${PROTO_OUTPUT}"
echo "  Duration : ${CAPTURE_SECONDS}s"
echo "========================================="
echo ""
echo "Make sure ALL ${MAX_TVS} TV(s) are POWERED OFF and visible to the camera."
printf "Press Enter when ready..."
read -r _

# ── Step 1: Delete existing prototype file ────────────────────────────────────

echo ""
echo "[1/7] Deleting existing prototype file..."
rm -f "$PROTO_OUTPUT" && echo "  Removed ${PROTO_OUTPUT}" || echo "  No existing file to remove"

# ── Step 2: Patch config — enable capture ────────────────────────────────────

echo "[2/7] Patching config (prototype_capture=true, max_tvs=${MAX_TVS})..."

# max_tvs
sed -i "s/\"max_tvs\"[[:space:]]*:[[:space:]]*[0-9]*/\"max_tvs\": ${MAX_TVS}/" "$CONFIG"

# prototype_capture: false -> true
sed -i "s/\"prototype_capture\"[[:space:]]*:[[:space:]]*false/\"prototype_capture\": true/" "$CONFIG"

# prototype_capture_seconds
sed -i "s/\"prototype_capture_seconds\"[[:space:]]*:[[:space:]]*[0-9]*/\"prototype_capture_seconds\": ${CAPTURE_SECONDS}/" "$CONFIG"

# Lower yolo_conf_thresh for capture — off TVs score lower than on TVs.
# Save original value so we can restore it.
ORIG_CONF=$(grep '"yolo_conf_thresh"' "$CONFIG" | grep -oE '[0-9]+\.[0-9]+' | head -1)
if [ -z "$ORIG_CONF" ]; then ORIG_CONF="0.18"; fi
sed -i "s/\"yolo_conf_thresh\"[[:space:]]*:[[:space:]]*[0-9.][0-9.]*/\"yolo_conf_thresh\": 0.12/" "$CONFIG"
echo "  Lowered yolo_conf_thresh: ${ORIG_CONF} -> 0.12 (restoring after capture)"

# prototype_labels — build JSON dynamically for any TV count
LABELS_JSON="{"
i=1
while [ "$i" -le "$MAX_TVS" ]; do
    if [ "$i" -gt 1 ]; then LABELS_JSON="${LABELS_JSON},"; fi
    LABELS_JSON="${LABELS_JSON} \"tv${i}\": \"TV_OFF\""
    i=$((i + 1))
done
LABELS_JSON="${LABELS_JSON} }"
# Replace the prototype_labels line (assumes it is on a single line in config.json)
sed -i "s|\"prototype_labels\"[[:space:]]*:[[:space:]]*{[^}]*}|\"prototype_labels\": ${LABELS_JSON}|" "$CONFIG"

echo "  Done — prototype_capture=true, labels=${LABELS_JSON}"

# ── Step 3: Stop + start in capture mode ─────────────────────────────────────

echo "[3/7] Stopping extension..."
"$BSEXT" stop 2>/dev/null || true
sleep 2

echo "[4/7] Starting extension in capture mode..."
"$BSEXT" start
echo ""
echo "  Capturing for ${CAPTURE_SECONDS} seconds — keep TVs powered OFF..."
i=0
while [ "$i" -lt "$CAPTURE_SECONDS" ]; do
    i=$((i + 5))
    sleep 5
    printf "  %d/%ds\n" "$i" "$CAPTURE_SECONDS"
done

# ── Step 4: Stop ─────────────────────────────────────────────────────────────

echo "[5/7] Stopping extension..."
"$BSEXT" stop 2>/dev/null || true
sleep 2

# ── Step 5: Verify ───────────────────────────────────────────────────────────

echo "[6/7] Verifying prototype file..."
if [ -f "$PROTO_OUTPUT" ]; then
    ls -lh "$PROTO_OUTPUT"
    echo "  Prototype file created successfully."
else
    echo ""
    echo "  WARNING: ${PROTO_OUTPUT} was not created!"
    echo "  Possible reasons:"
    echo "    - TVs were not detected (check yolo_conf_thresh in config)"
    echo "    - Extension crashed (check /var/log/anomaly_detection.log)"
    echo ""
    printf "  Continue and disable capture mode anyway? (y/N): "
    read -r CONT
    case "$CONT" in [Yy]) ;; *) exit 1 ;; esac
fi

# ── Step 6: Patch config — disable capture, restart ─────────────────────────

echo "[7/7] Disabling prototype_capture, restoring conf_thresh, restarting in normal mode..."
sed -i "s/\"prototype_capture\"[[:space:]]*:[[:space:]]*true/\"prototype_capture\": false/" "$CONFIG"
sed -i "s/\"yolo_conf_thresh\"[[:space:]]*:[[:space:]]*0\.12/\"yolo_conf_thresh\": ${ORIG_CONF}/" "$CONFIG"
echo "  prototype_capture=false, yolo_conf_thresh restored to ${ORIG_CONF}"

"$BSEXT" start

# ── Done ─────────────────────────────────────────────────────────────────────

echo ""
echo "========================================="
echo "  Capture complete"
echo "========================================="
echo "  Prototype file : ${PROTO_OUTPUT}"
echo "  Extension      : running (normal monitoring mode)"
echo "========================================="
