# brightsign-npu-anomaly-detection

NPU-accelerated video wall monitoring on BrightSign device using camera-based ROI extraction and **MobileNetV2 embedding** reference matching to detect black screens, freezes, stutter, and playback drift across multiple displays.

**Key Technology**: MobileNetV2 runs on the NPU to extract robust feature embeddings from each TV's ROI, enabling content-based anomaly detection without requiring face or object detection.

**Status**:

- **Embedding model validated on XT5 hardware** - NPU inference working perfectly with 1280-dim embeddings
- **Production C++ implementation complete** - Full multi-threaded pipeline with RGA+NPU acceleration (25 files: 13 headers, 12 sources)
- __Single unified executable__ - `anomaly_detection` with integrated build system

__Implementation__: Main entry point in `src/main.cpp`, supporting modules in `src/wvm/`, headers in `include/wvm/`. See [DESIGN.md](DESIGN.md) for architecture details.

## Build

This project uses a comprehensive CMake-based build system copied from [brightsign-npu-gaze-extension-ng](https://github.com/BrightSign-Playground/brightsign-npu-gaze-extension-ng).

**Recommended: full automated build (downloads SDK + builds all platforms)**

```bash
./scripts/runall.sh --auto
```

`--auto` runs all steps without prompting: downloads the BrightSign OS source,
builds the SDK in Docker, installs it to `./sdk/`, applies Rockchip library
patches, then builds the `anomaly_detection` binary for all platforms.

**Or build manually for a specific platform after the SDK is already installed:**

```bash
./build-apps XT5       # RK3588
./build-apps LS5       # RK3568
./build-apps Firebird  # RK3576
```

**Compile the MobileNetV2 ONNX model to RKNN format (required for NPU inference):**

```bash
./compile-models          # Compile for all platforms
./compile-models XT5      # Compile for XT5/RK3588 only
./compile-models LS5      # Compile for LS5/RK3568 only
./compile-models Firebird # Compile for Firebird/RK3576 only
```

If no calibration dataset exists at `toolkit/calibration_dataset.txt`, a synthetic
one is generated automatically (suitable for testing). For production deployments,
replace it with real frames from your video content:

```bash
python3 tools/create_calibration_dataset.py --videos ref1.mp4 ref2.mp4
```

**Then create the deployable extension package:**

```bash
./package                        # Package all platforms (dev + extension zips)
./package --ext-only             # Extension package only (production)
./package --soc RK3588           # Package a specific SOC only
./package --ext-only --soc RK3588 --verify  # Extension + post-package validation
```

This produces `anomaly-detection-ext-<timestamp>.zip`.

Other `runall.sh` options:

```bash
./scripts/runall.sh --help    # Show all options
./scripts/runall.sh --clean   # Remove all generated files, build dirs, Docker images
./scripts/runall.sh --skip-arch-check --auto  # Skip x86_64 check (for CI/testing)
```

See [DESIGN.md](DESIGN.md) for detailed build documentation.

## Start RTSP Stream

This section describes how to create a synthetic 5-TV health mosaic RTSP stream
on a development machine for testing the anomaly detection extension on an XT5.

### Overview

```ini
Dev machine (192.168.0.203)                   XT5 (192.168.0.165)
┌──────────────────────────────┐              ┌──────────────────────────────┐
│  MediaMTX (Docker) :8554     │              │  anomaly_detection           │
│                              │◄─── RTSP ───►│  config: health_5tv.json     │
│  make_mosaic_rtsp_...sh      │              │                              │
│  (ffmpeg → MediaMTX)         │              │  MQTT → mosquitto (127.0.0.1)│
└──────────────────────────────┘              └──────────────┬───────────────┘
                                                             │ MQTT
                                                             ▼
                                              mosquitto_sub -h 192.168.0.165
```

### Step 1: Start MediaMTX (RTSP server)

Run MediaMTX in Docker on your development machine:

```bash
docker run --rm -it -p 8554:8554 bluenviron/mediamtx:latest
```

MediaMTX listens for RTSP publishers on port `8554` and relays them to viewers.

#### Optional: mediamtx.yml for auto-start with ffmpeg

If you want MediaMTX to start the ffmpeg source automatically on container launch,
create a `mediamtx.yml` and mount it into the container:

```yaml
paths:
  live:
    # Start ffmpeg as soon as MediaMTX starts
    runOnInit: >
      ffmpeg -re -stream_loop -1 -i /home/sree/bs/anomaly/rtsp/Anomaly_Wallmart.mp4
             -an
             -vf scale=1920:1080,format=yuv420p
             -c:v libx264 -preset veryfast -tune zerolatency
             -profile:v high -level 4.1
             -b:v 2000k -maxrate 2000k -bufsize 1000k -g 50
             -fflags nobuffer -flags low_delay -muxdelay 0.1
             -f rtsp -rtsp_transport tcp rtsp://localhost:$RTSP_PORT/$MTX_PATH
    runOnInitRestart: yes
```

### Step 2: Create the 5-TV health mosaic stream

The script `rtsp/make_mosaic_rtsp_health_5tv_BlackOnly.sh` uses ffmpeg to synthesise
a 3×2 mosaic (1278×720) from a single input video with synthetic faults:

| Tile | TV ID | Simulated state |
|------|-------|-----------------|
| Row 1, Col 1 | tv1 | Clean video (OK) |
| Row 1, Col 2 | tv2 | Black window (BLACK) at t=8s for 4s |
| Row 1, Col 3 | tv4 | NO SIGNAL (OSD screen) |
| Row 2, Col 1 | tv3 | Always black (TV OFF) |
| Row 2, Col 2 | tv5 | HDMI2 input menu (WRONG_INPUT) |
| Row 2, Col 3 | —   | Blank filler |

Run the script:

```bash
./make_mosaic_rtsp_health_5tv_BlackOnly.sh Anomaly_Albertsons.mp4 rtsp://192.168.0.203:8554/live
```

- First argument: source video file (must be in the current directory)
- Second argument: RTSP publish URL (`192.168.0.203` = dev machine IP, `live` = stream path)

The script loops indefinitely (Ctrl+C to stop). While running the stream is
available at `rtsp://192.168.0.203:8554/live`.

#### Tunable parameters (environment variables)

```bash
TILE_W=426 TILE_H=360 FPS=30 \
BLACK_AT=8 BLACK_DUR=4 \
NOSIGNAL_TEXT="NO SIGNAL" HDMI_TEXT="HDMI2" \
./make_mosaic_rtsp_health_5tv_BlackOnly.sh Anomaly_Albertsons.mp4 rtsp://192.168.0.203:8554/live
```

<VSCode.Cell id="#VSC-install-hdr01" language="markdown">

## Install the Extension

</VSCode.Cell>
**1. Copy the package to the XT5:**

```bash
scp anomaly-detection-ext-*.zip brightsign@192.168.0.165:/storage/sd
```

**2. SSH in, unzip, and install:**

```bash
ssh brightsign@192.168.0.165
cd /storage/sd
/var/volatile/bsext/ext_npu_anomaly/bsext_init stop
unzip anomaly-detection-ext-*.zip
bash ./ext_npu_anomaly_install-lvm.sh
reboot
```

The installer mounts the squashfs extension and registers it with the BrightSign
extension manager. The extension starts automatically on the next reboot.

### Step 3: Configure the XT5

Ensure `config/health_5tv.json` on the XT5 points to the RTSP stream and
uses grid mode (no manual ROI coordinates needed):

```json
"device": {
  "camera_device": "rtsp://192.168.0.203:8554/live",
  "width": 1280,
  "height": 720,
  "fps": 10
},
"roi": {
  "mode": "grid",
  "grid": { "rows": 2, "cols": 3, "count": 5, "order": "row_major" }
}
```

Grid mode automatically assigns `tv1`..`tvN` to mosaic tiles in row-major order
(left→right, top→bottom). Set `count` to the actual number of TVs; tiles beyond
`count` are ignored.

#### Grid ROI reference for common TV counts

| TVs | `rows` | `cols` | `count` | Layout |
|-----|--------|--------|---------|--------|
| 1   | 1      | 1      | 1       | Single full-frame TV |
| 2   | 1      | 2      | 2       | Side-by-side |
| 2   | 2      | 1      | 2       | Stacked vertically |
| 3   | 1      | 3      | 3       | Single row of 3 |
| 3   | 2      | 2      | 3       | 2×2 grid, bottom-right tile unused |
| 4   | 2      | 2      | 4       | 2×2 full grid |
| 5   | 2      | 3      | 5       | 2×3 grid, bottom-right tile unused ← **default** |
| 6   | 2      | 3      | 6       | 2×3 full grid |
| 9   | 3      | 3      | 9       | 3×3 full grid |

**Example: 1 TV (full frame, 1280×720)**

```json
"roi": {
  "mode": "grid",
  "grid": { "rows": 1, "cols": 1, "count": 1, "order": "row_major" }
}
```

**Example: 2 TVs side-by-side (each tile 640×720)**

```json
"roi": {
  "mode": "grid",
  "grid": { "rows": 1, "cols": 2, "count": 2, "order": "row_major" }
}
```

**Example: 4 TVs in a 2×2 grid (each tile 640×360)**

```json
"roi": {
  "mode": "grid",
  "grid": { "rows": 2, "cols": 2, "count": 4, "order": "row_major" }
}
```

> To apply on the XT5 without rebuilding: copy your edited config to
> `/storage/sd/configs/config.json` (see §8.3) and restart the extension.

**Disable auto-start after reboot (optional):**

If you want the extension installed but not started automatically on boot, set
the registry flag from the BrightSign Serial/SSH console:

```sh
registry write extension bsext-anomaly-disable-auto-start true
```

To re-enable auto-start:

```sh
registry write extension bsext-anomaly-disable-auto-start false
```

To start/stop the extension manually at any time:

```bash
/var/volatile/bsext/ext_npu_anomaly/bsext_init start
/var/volatile/bsext/ext_npu_anomaly/bsext_init stop
/var/volatile/bsext/ext_npu_anomaly/bsext_init restart
/var/volatile/bsext/ext_npu_anomaly/bsext_init status
```

<VSCode.Cell id="#VSC-test-hdr01" language="markdown">

## Test and Verify

</VSCode.Cell>

### Start the extension and verify

SSH into the XT5 and start the extension:

```bash
ssh brightsign@192.168.0.165
/var/volatile/bsext/ext_npu_anomaly/bsext_init start
```

Check the log for successful RTSP connection and grid ROI generation:

```ini
[RTSP] ✅ Pipeline 1 opened successfully
[RTSP] ✅ First frame received: 1280x720
Preprocess: ROI mode=grid initial_count=0
Preprocess: ROI list regenerated. count=5 frame=1280x720
  ROI[0] tv1: x=0 y=0 w=426 h=360
  ROI[1] tv2: x=426 y=0 w=426 h=360
  ROI[2] tv3: x=852 y=0 w=428 h=360   ← last col absorbs remainder pixels
  ROI[3] tv4: x=0 y=360 w=426 h=360
  ROI[4] tv5: x=426 y=360 w=426 h=360
```

**Check logs:**

```bash
tail -f /tmp/anomaly_detection.log
```

**View live frame output (image-stream-server):**

The extension runs an HTTP image-stream server on port **20200**. While the extension is running, open a browser on any host in the same network:

```ini
http://192.168.0.165:20200/
```

This streams the latest annotated frame (with ROI overlays and anomaly scores) as a continuously refreshing JPEG. Useful for verifying ROI placement and confirming the detection pipeline is processing frames.

### Monitor MQTT health events

From any machine on the same network, subscribe to the health topic:

```bash
mosquitto_sub -h 192.168.0.165 -t 'videowall/health' -v
```

Expected output (published on state changes + every 30s heartbeat):

```json
videowall/health {"ts_ms":51407,"tv_id":"tv1","type":"HEALTH","details":{"health_state":"OK","luma_mean":102.0,"luma_var":7122.4,"dark_ratio":0.288}}
videowall/health {"ts_ms":51407,"tv_id":"tv2","type":"HEALTH","details":{"health_state":"OK","luma_mean":102.0,"luma_var":7126.2,"dark_ratio":0.288}}
videowall/health {"ts_ms":53774,"tv_id":"tv3","type":"HEALTH","details":{"health_state":"NO_SIGNAL","osd_similarity":0.994,"osd_label":"NO_SIGNAL"}}
videowall/health {"ts_ms":53774,"tv_id":"tv5","type":"HEALTH","details":{"health_state":"WRONG_INPUT","osd_similarity":0.993,"osd_label":"INPUT_MENU"}}
videowall/health {"ts_ms":54470,"tv_id":"tv4","type":"HEALTH","details":{"health_state":"TV_OFF","luma_mean":0.0,"dark_ratio":1.000}}
```

| TV | Expected state | How detected |
|----|---------------|--------------|
| tv1 | `OK` | Normal video, high variance |
| tv2 | `OK` → `BLACK` → `OK` | Black overlay at t=8s (`persist_black_ms=1500`) |
| tv3 | `NO_SIGNAL` | OSD prototype match (`osd_similarity≥0.85`) |
| tv4 | `TV_OFF` | `luma_mean≈0`, `dark_ratio=1.0` (`persist_off_ms=2000`) |
| tv5 | `WRONG_INPUT` | Input-menu OSD prototype match |

#### Pretty-print with jq

```bash
mosquitto_sub -h 192.168.0.165 -t 'videowall/health' -v | \
  while read topic msg; do echo "$msg" | jq '.'; done
```

## MobileNetV2 Model

This project uses **MobileNetV2** for embedding-based anomaly detection, replacing traditional object detection models.

**Why MobileNetV2 (not V3)?** MobileNetV2 is validated in the RKNN Model Zoo with documented performance on Rockchip SOCs, providing lower integration risk and proven reliability.

### Model Pipeline

```ini
Source Video → MobileNetV2 (ONNX) → RKNN → NPU Inference
     ↓                                   ↓
Reference Embeddings        Runtime Embedding Extraction
```

### Model Specifications

| Property | Value |
|----------|-------|
| Architecture | MobileNetV2 (ONNX Model Zoo) |
| Input Size | 224×224×3 RGB |
| Output | **1280-dim embedding** (✅ validated on XT5) |
| Quantization | INT8 asymmetric |
| Inference Time | ~10ms (RK3588), ~20ms (RK3568) |
| Model Size | 2.8 MB (RK3588), 2.5 MB (RK3568) |

**Models Available:**

- **Embedding:** `mobilenetv2-embedding-rk3588.rknn` - 1280-dim feature embeddings (✅ **VALIDATED ON XT5**)
- **Embedding:** `mobilenetv2-embedding-rk3568.rknn` - 1280-dim feature embeddings (✅ compiled)

**⚠️ IMPORTANT: Use the embedding model for anomaly detection!**

The embedding model provides **1280-dimensional feature vectors** which are optimal for similarity-based anomaly detection.

**Hardware Validation Results (XT5/RK3588):**

- ✅ Model loads: 2.74 MB
- ✅ NPU inference working perfectly
- ✅ Output: **1280 dimensions** (embedding layer)
- ✅ L2 Norm: ~32.28 (healthy range)
- ✅ Mean: 0.656, Std Dev: 0.619
- ✅ Sparsity: 5.5% (70 zeros out of 1280)
- ✅ **Ready for production use**

### Reference Timeline

For each video asset, generate a reference embedding timeline using the **validated 1280-dim embedding model**:

```python
# Offline preprocessing
embeddings = []
for frame in sample_frames(video, fps=5):
    roi = rectify(frame)  # 224x224
    emb = mobilenetv2_embedding(roi)  # 1280-dim embeddings (validated!)
    embeddings.append(emb)

# Save for runtime matching
save_reference(embeddings, "ref_embeddings.npy")
```

**Validated Hardware Characteristics (XT5):**

- Embedding dimension: 1280
- L2 norm range: ~30-35
- Mean: ~0.5-0.7
- Std dev: ~0.5-0.7
- Sparsity: ~5-10%

### Model Specifications

| Property | Value |
|----------|-------|
| Architecture | **MobileNetV2** (Rockchip-validated) |
| Source | ONNX Model Zoo |
| Input Size | 224×224×3 RGB |
| Output | **1280-dim embedding** (global average pooling) |
| Quantization | INT8 (100 calibration images) |
| Inference Time | ~10ms (RK3588), ~20ms (RK3568) |

### Why MobileNetV2?

We use **MobileNetV2** (not V3) because:

1. ✅ **Validated in RKNN Model Zoo** - End-to-end tested by Rockchip
2. ✅ **Lower Integration Risk** - Known to work well on target SOCs
3. ✅ **1280-dim Embeddings** - Rich feature vectors for matching
4. ✅ **Proven Performance** - Documented inference times and accuracy

## Project Structure

```ini
brightsign-npu-anomaly-detection/
├── src/                          # C++ source code
│   ├── main.cpp                  # Main entry point with signal handling
│   └── wvm/                      # Video wall monitor modules (14 files)
│       ├── config.cpp            # JSON config parser
│       ├── logger.cpp            # Logging system
│       ├── v4l2_capture.cpp      # USB camera V4L2 capture
│       ├── gst_rtsp_capture.cpp  # GStreamer RTSP capture (with reconnect)
│       ├── roi.cpp               # ROI management (grid + rect modes)
│       ├── rga_preproc.cpp       # RGA hardware preprocessing
│       ├── rknn_mobilenet.cpp    # RKNN NPU inference
│       ├── reference_db.cpp      # Reference embeddings
│       ├── matcher.cpp           # Windowed cosine matcher
│       ├── anomaly.cpp           # Anomaly detection state machines
│       ├── basic_anomaly.cpp     # Luma-based black/off/OSD detection
│       ├── health_engine.cpp     # TV health state engine
│       ├── mqtt.cpp              # MQTT publisher (auto-reconnect)
│       └── pipeline.cpp          # Multi-threaded orchestration
├── include/wvm/                  # Video wall monitor headers (17 files)
│   ├── types.hpp                 # Core data structures
│   ├── ts_queue.hpp              # Thread-safe queue
│   ├── config.hpp                # Configuration structures
│   ├── logger.hpp                # Logging interface
│   ├── capture.hpp               # Capture interface (ICapture)
│   ├── v4l2_capture.hpp          # V4L2 USB camera implementation
│   ├── gst_rtsp_capture.hpp      # GStreamer RTSP implementation
│   ├── roi.hpp                   # ROI management
│   ├── rga_preproc.hpp           # RGA preprocessing
│   ├── rknn_mobilenet.hpp        # RKNN model wrapper
│   ├── reference_db.hpp          # Reference database
│   ├── matcher.hpp               # Matcher interface
│   ├── anomaly.hpp               # Anomaly detection
│   ├── basic_anomaly.hpp         # Basic anomaly helpers
│   ├── health_engine.hpp         # Health engine
│   ├── mqtt.hpp                  # MQTT interface
│   └── pipeline.hpp              # Pipeline orchestrator
├── include/nlohmann/json.hpp     # JSON library (header-only)
├── config/                       # Configuration files
│   ├── health_5tv.json           # 5-TV health monitoring config
│   ├── config.json               # Default runtime config
│   ├── config_detect.json        # Detection-only config
│   ├── config_record.json        # Recording config
│   ├── osd_prototypes_template.json  # OSD prototype template
│   └── videowall.json            # Legacy videowall config
├── test_videos/                  # Test videos and mosaic script
│   ├── Anomaly_Albertsons.mp4
│   ├── Anomaly_Wallmart.mp4
│   └── make_mosaic_rtsp_health_5tv_BlackOnly.sh
├── scripts/                      # Build and deployment scripts
│   ├── runall.sh                 # Full automated build (SDK + all platforms)
│   ├── build_image_server.sh
│   ├── clean_build.sh
│   └── validate_bbappend.sh
├── toolkit/                      # RKNN model compilation toolkit
├── tools/                        # Host tools (reference builder, etc.)
├── CMakeLists.txt                # Unified CMake build configuration
├── build-apps                    # Multi-platform build script
├── compile-models                # RKNN model compilation script
├── package                       # Extension packaging script
├── gst-env.sh                    # GStreamer environment setup
├── Dockerfile                    # Docker build environment
├── DESIGN.md                     # System design and architecture
├── README.md                     # This file
└── manifest-config.template.json # Deployment manifest template
```

### Build Artifacts (Generated)

```ini
build_xt5/                        # XT5/RK3588 build directory
build_ls5/                        # LS5/RK3568 build directory
build_firebird/                   # Firebird/RK3576 build directory
install/                          # Installation directories
├── RK3588/
│   ├── bin/
│   │   └── anomaly_detection     # Single unified executable
│   ├── etc/
│   │   └── videowall.json        # Config
│   └── model/
│       └── mobilenetv2-embedding-rk3588.rknn
├── RK3568/
└── RK3576/
sdk/                              # BrightSign cross-compilation SDK
toolkit/                          # RKNN model compilation toolkit
```

> For the system design and architecture document, see [DESIGN.md](DESIGN.md).

