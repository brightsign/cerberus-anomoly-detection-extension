#!/bin/bash
# Quick diagnostic script to verify auto mode demo configuration

echo "=== Auto Mode Demo Configuration Check ==="
echo ""

CONFIG_FILE="/home/sree/bs/anomaly/brightsign-npu-anomaly-detection/config/config_auto_demo.json"

if [ ! -f "$CONFIG_FILE" ]; then
    echo "❌ ERROR: config_auto_demo.json not found!"
    exit 1
fi

echo "✅ Config file exists: $CONFIG_FILE"
echo ""

echo "=== ROI Configuration ===" 
echo "Checking for tv1 and tv2 ROIs..."
if grep -q '"id": "tv1"' "$CONFIG_FILE" && grep -q '"id": "tv2"' "$CONFIG_FILE"; then
    echo "✅ Found both tv1 and tv2 ROIs"
    echo ""
    echo "tv1 (reference):"
    grep -A 4 '"id": "tv1"' "$CONFIG_FILE" | grep -E '"x"|"y"|"w"|"h"'
    echo ""
    echo "tv2 (test):"
    grep -A 4 '"id": "tv2"' "$CONFIG_FILE" | grep -E '"x"|"y"|"w"|"h"'
else
    echo "❌ ERROR: Missing tv1 or tv2 ROI definitions!"
fi
echo ""

echo "=== Reference Mode ===" 
if grep -q '"mode": "auto"' "$CONFIG_FILE"; then
    echo "✅ Auto mode enabled"
    echo "   ref_tv_id: $(grep 'ref_tv_id' "$CONFIG_FILE" | head -1)"
    echo "   auto_source: $(grep 'auto_source' "$CONFIG_FILE" | head -1)"
else
    echo "❌ ERROR: Auto mode not configured!"
fi
echo ""

echo "=== Test Videos Available ===" 
if [ -f "/home/sree/bs/anomaly/Drinks.mp4" ]; then
    echo "✅ Clean video: /home/sree/bs/anomaly/Drinks.mp4"
else
    echo "❌ Missing: Drinks.mp4"
fi

if [ -f "/home/sree/bs/anomaly/Drinks_with_anomalies.mp4" ]; then
    echo "✅ Anomaly video: /home/sree/bs/anomaly/Drinks_with_anomalies.mp4"
else
    echo "❌ Missing: Drinks_with_anomalies.mp4"
fi
echo ""

echo "=== How to Run Demo ===" 
echo ""
echo "1. On Monitor 1 (left/tv1): Play clean video"
echo "   ffplay -loop 0 /home/sree/bs/anomaly/Drinks.mp4"
echo ""
echo "2. On Monitor 2 (right/tv2): Play video with anomalies"  
echo "   ffplay -loop 0 /home/sree/bs/anomaly/Drinks_with_anomalies.mp4"
echo ""
echo "3. Start the extension with auto mode config:"
echo "   cd /home/sree/bs/anomaly/brightsign-npu-anomaly-detection"
echo "   ./scripts/runall.sh --auto"
echo ""
echo "   OR manually:"
echo "   ./install/videowall-monitor config/config_auto_demo.json"
echo ""
echo "4. Monitor MQTT events:"
echo "   mosquitto_sub -h 127.0.0.1 -t videowall/events -v"
echo ""
echo "=== Expected Detection Timeline ==="
echo "0:00-3:00   Warmup (building reference, BLACK detection active)"
echo "3:00-5:00   Normal (reference armed, all detection active)"
echo "5:00-8:00   FREEZE on tv2 detected"
echo "8:00-10:00  Normal (recovered)"
echo "10:00-12:00 BLACK on tv2 detected"
echo "12:00-16:48 Normal (recovered)"
echo ""
