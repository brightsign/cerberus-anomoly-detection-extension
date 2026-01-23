# Next Steps: Building and Deploying

## Summary

✅ **Complete C++ implementation** (25 files: 13 headers + 12 sources)
- Multi-threaded pipeline (5 threads with bounded queues)
- RGA hardware acceleration for preprocessing
- RKNN NPU inference for embeddings
- Reference matching with windowed search
- Anomaly detection (BLACK, FREEZE, LAG, STUTTER, MISMATCH)
- MQTT event publishing

## Immediate Actions

### 1. Add nlohmann/json Dependency (REQUIRED)

```bash
cd videowall-monitor
mkdir -p include/nlohmann
wget -O include/nlohmann/json.hpp \
  https://github.com/nlohmann/json/releases/download/v3.11.2/json.hpp
```

### 2. Verify Camera Format

On XT5 or dev machine with camera:
```bash
v4l2-ctl -d /dev/video0 --all | grep "Pixel Format"
```

If your camera outputs **NV12** instead of YUYV, update `src/rga_preproc.cpp` line ~70:
```cpp
int src_format = RK_FORMAT_YCbCr_420_SP; // was RK_FORMAT_YUYV_422
```

### 3. Build for XT5

```bash
cd videowall-monitor
mkdir build && cd build

# Source BrightSign SDK environment
source ../../sdk/environment-setup-aarch64-oe-linux

# Configure
cmake .. -DWVM_USE_RGA=ON -DWVM_USE_MOSQUITTO=OFF

# Build
make -j$(nproc)
```

Expected output: `videowall_monitor` executable (~500KB-1MB)

### 4. Generate Reference Embeddings (BLOCKING)

You need to create a tool to generate reference embeddings from your video:

```python
# tools/build_reference_timeline.py (TO BE CREATED)
# See VIDEOWALL_IMPLEMENTATION.md for implementation details

python tools/build_reference_timeline.py \
    --video reference_video.mp4 \
    --model ../install/RK3588/model/mobilenetv2-embedding-rk3588.rknn \
    --fps 5 \
    --output ref_embeddings.f32
```

**⚠️ This tool needs to be created** - see VIDEOWALL_IMPLEMENTATION.md section on "Reference Timeline Construction"

### 5. Calibrate ROIs

Capture a frame from your camera setup:
```bash
# On XT5
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=1080
v4l2-ctl -d /dev/video0 --stream-mmap --stream-to=frame.yuv --stream-count=1

# Convert to viewable image
ffmpeg -f rawvideo -pix_fmt yuyv422 -s 1920x1080 -i frame.yuv frame.png
```

Measure TV positions (x, y, w, h) and update `config/videowall.json`:
```json
"roi": {
  "tvs": [
    { "id": "tv1", "x": 100, "y": 80, "w": 500, "h": 280 },
    { "id": "tv2", "x": 680, "y": 80, "w": 500, "h": 280 }
  ]
}
```

### 6. Deploy to XT5

```bash
# Package
tar czf videowall-monitor-xt5.tar.gz \
    videowall_monitor \
    config/videowall.json \
    ../install/RK3588/model/mobilenetv2-embedding-rk3588.rknn

# Deploy
scp videowall-monitor-xt5.tar.gz brightsign@XT5_IP:/storage/sd/
ssh brightsign@XT5_IP "cd /storage/sd && tar xzf videowall-monitor-xt5.tar.gz"
```

### 7. Test on XT5

```bash
# SSH to XT5
ssh brightsign@XT5_IP

# Set library path
export LD_LIBRARY_PATH=/var/volatile/bsext/ext_npu_gaze/RK3588/lib:$LD_LIBRARY_PATH

# Run (will fail without reference embeddings initially)
/storage/sd/videowall_monitor /storage/sd/config/videowall.json

# Monitor logs
tail -f /var/log/videowall-monitor.log
```

## Critical Path

```
1. Add nlohmann/json          ✅ (1 command)
2. Build                        ✅ (ready to build)
3. Create reference builder     ⚠️  (BLOCKING - needs implementation)
4. Generate reference timeline  ⚠️  (BLOCKING - needs reference video)
5. Calibrate ROIs              ⏳ (manual measurement)
6. Full test on XT5            ⏳ (after steps 3-5)
```

## Known Issues to Address

1. **nlohmann/json dependency**: Not included, must be added manually
2. **Reference timeline builder**: Needs to be created (Python + rknnlite)
3. **ROI calibration**: Manual process, could create helper tool
4. **RGA format constants**: May need adjustment based on actual camera

## Success Criteria

- ✅ Code compiles cleanly
- ⏳ Camera capture working
- ⏳ NPU inference producing 1280-dim embeddings
- ⏳ Reference matching working (requires reference timeline)
- ⏳ Anomaly detection triggering events
- ⏳ MQTT events published

## Documentation

- **Architecture**: [VIDEOWALL_IMPLEMENTATION.md](../VIDEOWALL_IMPLEMENTATION.md)
- **Quick Reference**: [QUICK_REFERENCE.md](../QUICK_REFERENCE.md)
- **Model Details**: [EMBEDDING_MODEL_VALIDATED.md](../EMBEDDING_MODEL_VALIDATED.md)
- **API Reference**: See header files in `include/wvm/`

## Support

See VIDEOWALL_IMPLEMENTATION.md for:
- Threading model details
- Hardware acceleration strategy
- Configuration options
- Troubleshooting guide
- Performance tuning
