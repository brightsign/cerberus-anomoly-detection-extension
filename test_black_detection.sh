#!/bin/bash
# Quick test script for BLACK screen detection

echo "============================================"
echo "BLACK Screen Detection Test"
echo "============================================"
echo ""
echo "This script will help you test the BLACK screen detection feature."
echo ""

# Check if mosquitto_sub is available
if ! command -v mosquitto_sub &> /dev/null; then
    echo "WARNING: mosquitto_sub not found in PATH"
    echo "You may need to use the bundled version or install mosquitto-clients"
fi

echo "Step 1: Checking if services are running..."
if pgrep -f "anomaly_detection" > /dev/null; then
    echo "  ✓ anomaly_detection is running"
else
    echo "  ✗ anomaly_detection is NOT running"
    echo ""
    echo "Starting services..."
    cd "$(dirname "$0")"
    ./bsext_init start
    sleep 3
fi

if pgrep -f "mosquitto" > /dev/null; then
    echo "  ✓ MQTT broker is running"
else
    echo "  ✗ MQTT broker is NOT running"
    echo "  Please run: ./bsext_init start"
    exit 1
fi

echo ""
echo "Step 2: Current configuration:"
echo "  black_luma_mean threshold: 12.0"
echo "  black_luma_var threshold: 8.0"
echo "  persist_black_ms: 1500 (1.5 seconds)"
echo ""
echo "Step 3: Testing instructions:"
echo "  1. Point your USB camera at a BLACK monitor/screen"
echo "  2. Keep it steady for at least 1.5 seconds"
echo "  3. Watch for BLACK event below"
echo "  4. Point camera away from black screen"
echo "  5. Watch for RECOVERED event"
echo ""
echo "Step 4: Subscribing to MQTT events..."
echo "  Topic: videowall/events"
echo "  Press Ctrl+C to exit"
echo ""
echo "============================================"
echo "MQTT Event Monitor (waiting for events...):"
echo "============================================"

# Try mosquitto_sub, fallback to bundled version if needed
if command -v mosquitto_sub &> /dev/null; then
    mosquitto_sub -h 127.0.0.1 -p 1883 -t 'videowall/events' -v
else
    # Try bundled version
    SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
    if [ -f "${SCRIPT_DIR}/RK3588/bin/mosquitto_sub" ]; then
        export LD_LIBRARY_PATH="${SCRIPT_DIR}/RK3588/bin:${SCRIPT_DIR}/RK3588/lib:${LD_LIBRARY_PATH}"
        ${SCRIPT_DIR}/RK3588/bin/mosquitto_sub -h 127.0.0.1 -p 1883 -t 'videowall/events' -v
    else
        echo "ERROR: mosquitto_sub not found!"
        echo "Please install mosquitto-clients or check your installation"
        exit 1
    fi
fi
