# brightsign-npu-anomaly-detection

NPU-accelerated video wall monitoring on BrightSign device using camera-based ROI extraction and **MobileNetV3 embedding** reference matching to detect black screens, freezes, stutter, and playback drift across multiple displays.

**Key Technology**: MobileNetV3 runs on the NPU to extract robust feature embeddings from each TV's ROI, enabling content-based anomaly detection without requiring face or object detection.

## Build System

This project uses a comprehensive CMake-based build system copied from [brightsign-npu-gaze-extension-ng](https://github.com/BrightSign-Playground/brightsign-npu-gaze-extension-ng).

### Quick Start

```bash
# Install BrightSign SDK first
./brightsign-x86_64-*-toolchain-*.sh -d ./sdk -y

# Build for all platforms
./build-apps

# Or build for specific platform
./build-apps XT5       # RK3588
./build-apps LS5       # RK3568  
./build-apps Firebird  # RK3576
```

### Components

- **CMakeLists.txt** - Cross-compilation configuration
- **build-apps** - Multi-platform build script
- **gst-env.sh** - GStreamer environment setup
- **scripts/** - Helper scripts for build and deployment

See [BUILD_SYSTEM.md](BUILD_SYSTEM.md) for detailed documentation.

## MobileNetV3 Model

This project uses **MobileNetV3** for embedding-based anomaly detection, replacing traditional object detection models.

### Model Pipeline

```
Source Video → MobileNetV3 (PyTorch/TF) → ONNX → RKNN → NPU Inference
     ↓                                         ↓
Reference Embeddings              Runtime Embedding Extraction
```

### Quick Start

```bash
# 1. Export pretrained MobileNetV3 to ONNX
python tools/export_mobilenetv3_onnx.py

# 2. Convert to RKNN for target platform
python tools/convert_to_rknn.py --platform rk3588

# 3. Build reference timeline from video
python tools/build_reference.py --video reference.mp4 --fps 5

# 4. Place models in install directory
mkdir -p install/RK3588/model
cp mobilenetv3_large_rk3588.rknn install/RK3588/model/
```

### Model Specifications

| Property | Value |
|----------|-------|
| Architecture | MobileNetV3-Large |
| Input Size | 224×224×3 RGB |
| Output | 960-dim embedding |
| Quantization | int8 (asymmetric) |
| Inference Time | ~10ms (RK3588) |
| Memory | ~5MB model size |

### Reference Timeline

For each video asset, generate a reference embedding timeline:

```python
# Offline preprocessing
embeddings = []
for frame in sample_frames(video, fps=5):
    roi = rectify(frame)  # 224x224
    emb = mobilenetv3(roi)  # 960-dim
    embeddings.append(emb)

# Save for runtime matching
save_reference(embeddings, "ref_embeddings.npy")
```

See [MOBILENETV3_INTEGRATION.md](MOBILENETV3_INTEGRATION.md) for detailed conversion and deployment instructions.

## Project Structure

```
brightsign-npu-anomaly-detection/
├── src/                          # C++ source code
│   └── main.cpp                  # Main entry point (placeholder)
├── include/                      # Header files
├── configs/                      # Configuration files
├── scripts/                      # Build and deployment scripts
│   ├── build_image_server.sh
│   ├── check-config-reload.sh
│   ├── clean_build.sh
│   ├── install-gstreamer-plugins.sh
│   ├── rebuild_gstreamer.sh
│   ├── runall.sh
│   └── validate_bbappend.sh
├── CMakeLists.txt                # CMake build configuration
├── build-apps                    # Multi-platform build script
├── gst-env.sh                    # GStreamer environment
├── .gitignore                    # Git ignore rules
├── BUILD_SYSTEM.md               # Build system documentation
├── README.md                     # This file
└── manifest-config.template.json # Deployment manifest template
```

### Build Artifacts (Generated)

```
build_xt5/                        # XT5/RK3588 build directory
build_ls5/                        # LS5/RK3568 build directory
build_firebird/                   # Firebird/RK3576 build directory
install/                          # Installation directories
├── RK3588/
├── RK3568/
└── RK3576/
sdk/                              # BrightSign cross-compilation SDK
toolkit/                          # RKNN model compilation toolkit
```

## Architecture Overview

### Anomaly Detection Pipeline

```
Camera Input → ROI Extraction → Embedding Extraction → Reference Matching → Anomaly Detection
     ↓              ↓                    ↓                      ↓                    ↓
  GStreamer    OpenCV/RGA         MobileNetV3 (NPU)      Similarity Compare    Event Publisher
```

### MobileNetV3 Model

**Why MobileNetV3?**
- **Embedding Extraction**: Generates rich 960-dim feature vectors per TV ROI
- **NPU Optimized**: ~10ms inference time on RK3588
- **Robust**: Handles camera exposure, reflections, and minor distortions
- **Content-Agnostic**: Works with any video content (no face/object detection needed)
- **Efficient**: Lightweight architecture ideal for multi-TV monitoring

**Model Configuration:**
- Input: 224×224×3 RGB (rectified ROI)
- Output: 960-dimensional embedding vector
- Quantization: int8 for NPU acceleration
- Inference: ~10ms per ROI on RK3588

See [MOBILENETV3_INTEGRATION.md](MOBILENETV3_INTEGRATION.md) for complete model pipeline details.

### Key Components (To Be Implemented)

1. **Input Module**
   - Camera capture (USB, RTSP)
   - ROI extraction from video wall displays
   - Frame preprocessing with RGA acceleration

2. **Model Module**
   - **MobileNetV3 embedding extraction** (primary model)
   - NPU-accelerated inference via RKNN
   - Reference embedding management

3. **Detection Module**
   - Embedding similarity comparison (cosine similarity)
   - Black screen detection (luma variance)
   - Freeze detection (temporal embedding consistency)
   - Stutter/drift detection (reference timeline alignment)
   - Threshold-based anomaly classification

4. **Output Module**
   - MQTT event publishing
   - Local logging
   - Optional visualization
   - Alert notifications

### Supported Platforms

- **XT5** (RK3588) - High-performance NPU (Recommended)
- **LS5** (RK3568) - Mid-range NPU
- **Firebird** (RK3576) - Latest generation NPU

### Hardware Acceleration

- **NPU** - MobileNetV3 embedding inference (RKNN)
- **RGA** - 2D graphics acceleration (ROI warping, format conversion)
- **MPP** - Video decode acceleration (if using RTSP)
- **GStreamer** - Hardware-accelerated video pipeline

## Yocto/BitBake Recipes

The `bsoe-recipes/` directory contains Yocto BitBake recipes for building the BrightSign SDK.

### SDK Generation

```bash
# Source Yocto environment
source brightsign-oe/oe-init-build-env

# Add meta-bs layer
bitbake-layers add-layer ../bsoe-recipes/meta-bs

# Build SDK for target platform
MACHINE=rk3588 bitbake brightsign-sdk -c populate_sdk
```

### Key Recipes

- **brightsign-sdk.bb** - Generates cross-compilation SDK with all dependencies
- **ext-bundle.bb** - Packages GStreamer runtime for extension deployment
- **opencv_4.5.5.bb** - Builds OpenCV with hardware acceleration
- **GStreamer bbappends** - Configures GStreamer plugins for RTSP streaming

### SDK Contents

The generated SDK includes:
- Cross-compilation toolchain (x86_64 → aarch64)
- OpenCV 4.5.5 with all modules
- GStreamer 1.0 with RTSP/RTP plugins
- MQTT (mosquitto) client and broker
- RGA library (Rockchip Graphics Accelerator)
- TurboJPEG, libwebsockets, and other dependencies

See [bsoe-recipes/README.md](bsoe-recipes/README.md) for complete documentation.

## Design Document

# Video Wall Monitor (BrightSign XT5 Extension) — Design

## 1. Purpose

This extension monitors a wall/array of TVs that are expected to display the same signage content. A single camera connected to a BrightSign XT5 observes the wall. The system detects and localizes playback issues per screen, including:

- Black/blank screen
- Frozen video
- Playback drift / lag relative to peers and/or a known reference asset
- Stutter/judder (irregular progression)
- Wrong content / mismatch

A lightweight CNN (MobileNetV3) runs on the XT5 NPU to generate per-screen embeddings that support robust matching under real-world camera artifacts (exposure changes, reflections, minor misalignment).

---

## 2. Goals and Non-Goals

### Goals
- Per-TV health determination from a single camera view.
- Real-time (or near real-time) detection with tunable sensitivity.
- NPU-accelerated inference; CPU used only for lightweight orchestration and vector math.
- Operate as a BrightSign OS Extension (read-only squashfs + init script).
- Support known reference video matching (asset-based alignment), plus peer-consensus detection.

### Non-Goals (initial release)
- Perfect sub-frame synchronization measurement.
- Root-cause diagnosis beyond visual symptoms (e.g., HDMI cable vs panel failure).
- Automatic TV ROI discovery in arbitrary environments (initially manual calibration).

---

## 3. Assumptions and Constraints

- **Platform**: BrightSign XT5 (Series 5) running BrightSignOS (BOS) with NPU available.
- **Camera input**: Start with **USB webcam** input.
- **Wall layout**: TVs are fixed; camera is fixed; ROIs can be calibrated once and reused.
- **Reference**: Source video(s) are known (single asset or playlist) and can be preprocessed offline.
- **Extension**: Runs from read-only extension mount; writes logs/state to writable locations.

---

## 4. High-Level Architecture

### Runtime Components (on XT5)

1. **Capture**
   - Captures frames from USB camera (V4L2/Media pipeline).
   - Produces frames `F(t)` with timestamps.

2. **ROI Extract + Rectify**
   - For each TV `i`, crops and warps a polygon ROI into a canonical rectangle using a homography `H_i`.
   - Outputs rectified ROI images `R_i(t)`.

3. **Prechecks (cheap CV)**
   - Computes per-ROI luma mean/variance to quickly detect black/blank and optionally throttle inference.

4. **NPU Inference (MobileNetV3 RKNN)**
   - Runs MobileNetV3 as an **embedding extractor** (classifier head removed/ignored).
   - Outputs per-ROI embedding vectors `E_i(t)`.

5. **Reference Matcher**
   - Aligns each `E_i(t)` to a precomputed reference embedding timeline `E_ref[k]`.
   - Produces best-match index `k*_i(t)` and similarity `S_i(t)`.

6. **Consensus + Anomaly Engine**
   - Computes wall consensus position (median/cluster) and per-TV lag estimates.
   - Evaluates state machines for BLACK, FREEZE, LAG, STUTTER, MISMATCH.

7. **Local API + Dashboard**
   - Exposes status, configuration, and recent events via local HTTP.
   - Optional export to syslog/MQTT/webhook.

### Offline/Build-Time Tools (host machine)
8. **Reference Builder**
   - Takes source video asset(s), samples frames, runs the same embedding model, and generates:
     - `ref_embeddings.bin` (or `.npy`)
     - `ref_index.json` (frame index ↔ timestamp, metadata)
     - optional `ref_informativeness.bin` (confidence mask per frame)

9. **ROI Calibrator**
   - Interactive tool to define TV polygons/corners in a captured image.
   - Generates `wall_layout.json` with polygons + homographies.

---

## 5. Data Flow Diagram

```
USB Camera -> Frame F(t)
|
v
+-------------------+
| ROI Rectification |
|  R_i(t)=warp(F,H) |
+-------------------+
|
+-----+-------------------------------+
|                                     |
v                                     v
Luma/Variance check                  NPU Embedding
(BLACK prefilter)                 E_i(t)=MobileNetV3(R_i)
|                                     |
+------------------+------------------+
v
Reference Matching
k*_i(t), S_i(t) vs E_ref[k]
|
v
Consensus + Anomaly Engine
BLACK / FREEZE / LAG / STUTTER / MISMATCH
|
v
API + Dashboard + Alerts
```

---

## 6. Core Algorithms

### 6.1 ROI Rectification
Each TV is represented by a 4-corner polygon in camera coordinates.

- Store `P_i = {p0, p1, p2, p3}` (clockwise corners).
- Compute homography `H_i` mapping `P_i` → canonical rectangle (e.g., 224×224).
- Rectify each frame: `R_i(t) = warpPerspective(F(t), H_i)`.

**Note**: Inset crop by a small margin (e.g., 2–5%) to avoid bezel edges and reduce false mismatches.

---

### 6.2 MobileNetV3 Embedding (NPU)
MobileNetV3 runs on the NPU via RKNN.

**Input**: `R_i(t)` resized to model input (e.g., 224×224 RGB).  
**Output**: embedding vector `E_i(t)` (e.g., 256–1024 dims depending on chosen tap point/projection).

**Why embeddings**:
- More invariant to camera exposure and mild distortions than pixel/SSIM/hash.
- Fast to compare across many TVs and against reference timeline.

---

### 6.3 Reference Timeline Construction (Offline)
For each reference asset:
- Choose `ref_fps` (2–10 fps).
- Sample frames, compute embeddings using the same model:
  - `E_ref[k] = Embedding(frame_k)`
- Record `t_ref[k] = k / ref_fps`.

Optional but strongly recommended:
- Compute `info[k]` (informativeness), e.g. luma variance or edge energy.
  - Low `info[k]` indicates visually ambiguous frames (dark/flat scenes).
  - Use this to dampen mismatch alarms in low-confidence sections.

---

### 6.4 Runtime Sync (Alignment) Without PTS
PTS is **not required**. We align using content:

Per TV `i`, keep a state estimate of the current reference index `k_i`.

At each sample:
1. Predict:
   - `k_pred = k_i + round(ref_fps / sample_fps)`
2. Windowed search:
   - Search `k` in `[k_pred - W, k_pred + W]` (W is in frames, e.g., ±2 seconds worth).
3. Select best match:
   - `k*_i(t) = argmax_k cosine(E_i(t), E_ref[k])`
   - `S_i(t) = max_k cosine(...)`
4. Update:
   - `k_i ← k*_i(t)` with smoothing/continuity constraints.

**Continuity constraints**:
- Disallow large jumps unless similarity strongly supports it.
- Handle loop boundaries by allowing wrap-around.

---

### 6.5 Wall Consensus and Relative Lag
For stability and better drift detection, compute a consensus reference position:

- `k_wall(t) = median_i(k*_i(t))` over TVs currently "confident" (S above threshold and info high).
- Per TV lag:
  - `lag_i(t) = (k_wall(t) - k*_i(t)) / ref_fps` seconds

This allows drift detection even if schedule timing is unknown.

---

## 7. Anomaly Detection (State Machines)

All anomalies use persistence timers to avoid single-frame noise.

### 7.1 BLACK / BLANK
Condition:
- `mean_luma(R_i) < L_dark` AND `var_luma(R_i) < V_low` for `T_black`.

Guard:
- If most TVs are simultaneously dark and reference frames are low-informativeness, suppress.

Output:
- `BLACK(i)` event; include ROI snapshot.

---

### 7.2 FREEZE (Stuck Frame)
Signals:
- `k*_i(t)` does not advance while `k_wall(t)` advances, for `T_freeze`, OR
- `dist(E_i(t), E_i(t-1)) < ε` for `T_freeze`.

Output:
- `FREEZE(i)`; include duration and last advancing index.

---

### 7.3 LAG / DRIFT
Condition:
- `lag_i(t) > lag_threshold` seconds for `T_lag`.

Output:
- `LAG(i)` with lag seconds and trend.

---

### 7.4 STUTTER / JUDDER
Compute index derivative:
- `Δk_i(t) = k*_i(t) - k*_i(t-1)`

Stutter indicators over a sliding window:
- High fraction of `Δk = 0` (repeats) plus occasional large positive jumps.
- High variance of `Δk` compared to peers.

Output:
- `STUTTER(i)` with stutter score and recent `Δk` trace.

---

### 7.5 MISMATCH / WRONG CONTENT
Condition:
- `S_i(t) < S_min` for `T_mismatch` during informative reference sections (`info[k_wall] >= info_min`), OR
- Best-match index oscillates non-continuously with low similarity.

Output:
- `MISMATCH(i)`; include similarity trend and snapshot.

---

## 8. Configuration

### 8.1 Static Config Files
- `wall_layout.json`
  - TV polygons/corners
  - homographies (optional precomputed)
  - camera intrinsic/extrinsic notes (optional)
- `thresholds.json`
  - fps parameters
  - luma thresholds
  - similarity thresholds
  - persistence timers
  - search window sizes
- `references/<asset_id>/ref_index.json`
  - ref_fps, duration, loop info
  - mapping indices to time
  - hash/signature of the source asset
- `references/<asset_id>/ref_embeddings.bin`
  - embeddings array (float16 or int8)

### 8.2 Runtime Overrides (Writable)
- `/var/volatile/videowall-monitor/runtime.json`
- Used for temporary tuning without repackaging extension.

---

## 9. Performance and Resource Strategy

### Key levers
- `sample_fps`: how often to analyze (2–10 fps).
- `roi_size`: input resolution per TV (e.g., 160–224 square).
- `N`: number of TVs.

### Recommendations (starting point)
- `sample_fps = 5`, `ref_fps = 5`
- `roi_size = 192` or `224` depending on camera resolution and distance
- Search window `W = ±2s` initially (±10 frames at 5 fps)

### Compute placement
- NPU: embedding inference.
- CPU: homography warp, luma stats, cosine similarity, state machines.
  - Vector comparisons are inexpensive; the primary CPU load is ROI warping.
  - If ROI warping becomes heavy, reduce ROI size and/or sample_fps.

---

## 10. Extension Packaging and Runtime Layout

### Extension filesystem (read-only)
- `/var/volatile/bsext/videowall-monitor/`
  - `bin/videowall-monitor`
  - `etc/videowall-monitor/*.json`
  - `share/videowall-monitor/models/*.rknn`
  - `share/videowall-monitor/references/...`

### Writable runtime directories
- Logs: `/var/log/videowall-monitor/` (or `/var/volatile/...`)
- State/ring buffers: `/var/volatile/videowall-monitor/`

### Init/Service lifecycle
- `bsext_init start|stop|restart|status`
- Watchdog policy:
  - restart daemon if it exits
  - emit a "monitor offline" event if unavailable

---

## 11. Build and Toolchain Integration (SDK via BitBake)

### Build approach (recommended)
- Use a prebuilt BrightSign SDK generated by BitBake.
- Compile daemon with CMake using SDK toolchain/sysroot.
- Convert model to RKNN and package as an asset.
- Stage files into extension root and build squashfs extension image.

### Model pipeline
- Train/choose MobileNetV3 (often pretrained) on host.
- Export to ONNX.
- Convert + quantize to RKNN with a representative calibration set.
- Validate output tensor shape and accuracy on representative wall images.

---

## 12. Testing Strategy

### Offline (host)
- Reference builder correctness (embedding timeline length, index mapping).
- Replay recorded camera footage:
  - inject synthetic faults (black/freeze/lag/stutter/wrong content)
  - validate detection and latency

### On-device (XT5)
- USB camera acquisition stability.
- ROI calibration validation (visual overlay).
- Inference throughput under worst-case `N`.
- Long-run soak test (24–72 hours) for leaks and drift.

### Acceptance criteria (initial)
- Detect black screen within `<= 2s`.
- Detect freeze within `<= 3s`.
- Detect lag > 2s within `<= 5s`.
- Detect mismatch within `<= 5s` (excluding low-informativeness segments).

---

## 13. Risks and Mitigations

1. **USB camera variability / exposure flicker**
   - Prefer cameras with manual exposure lock.
   - Add smoothing and peer-consensus gating.

2. **Moiré / rolling shutter banding**
   - Use embeddings (more robust than pixel hashes).
   - Avoid bezel edges; downscale appropriately.

3. **Dark scenes cause ambiguous matching**
   - Use informativeness mask in reference; suppress mismatch alarms there.
   - Lean on continuity constraints and peer consensus.

4. **Playlist / asset changes**
   - Store references per asset_id and switch references via schedule/config.

---

## 14. Future Enhancements

- Auto-ROI discovery (markers, ArUco/AprilTag, or bezel detection).
- Multi-camera support for large walls.
- Stabilization / registration for slight camera movement.
- Server aggregation for fleet-wide monitoring and long-term analytics.
- Optional on-device classifier for "NO SIGNAL / COLOR BARS / ERROR OVERLAY" states.
