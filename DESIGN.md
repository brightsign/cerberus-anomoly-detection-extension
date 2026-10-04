# Video Wall Monitor (BrightSign XT5 Extension) — Design

## 1. Purpose

This extension monitors a wall/array of TVs that are expected to display the same signage content. A single camera connected to a BrightSign XT5 observes the wall. The system detects and localizes playback issues per screen, including:

- Black/blank screen
- Frozen video
- Playback drift / lag relative to peers and/or a known reference asset
- Stutter/judder (irregular progression)
- Wrong content / mismatch

A lightweight CNN (MobileNetV2) runs on the XT5 NPU to generate per-screen embeddings that support robust matching under real-world camera artifacts (exposure changes, reflections, minor misalignment).

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

4. **NPU Inference (MobileNetV2 RKNN)**

   - Runs MobileNetV2 as an **embedding extractor** (classifier head removed/ignored).
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

```sh
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
(BLACK prefilter)                 E_i(t)=MobileNetV2(R_i)
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

### 6.2 MobileNetV2 Embedding (NPU)

MobileNetV2 runs on the NPU via RKNN.

__Input__: `R_i(t)` resized to model input (e.g., 224×224 RGB).  
__Output__: embedding vector `E_i(t)` (1000-dim classification or 1280-dim embedding depending on output layer configuration).

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

### 8.3 Config Override (SD Card)

The extension ships a default config embedded in the squashfs image. To override
settings without rebuilding the extension, place a `config.json` on the XT5's
SD card:

```sh
/storage/sd/configs/config.json
```

On startup the extension checks for this file and uses it in place of the
bundled defaults. This is the recommended way to customise deployment settings
(RTSP URL, ROI layout, thresholds) in the field without touching the extension
package.

**Workflow:**

1. Edit a local copy of `config/health_5tv.json` (or any config in `config/`).
2. Copy it to the XT5 SD card:

```bash
scp config/health_5tv.json brightsign@192.168.0.165:/storage/sd/configs/config.json
```

3. Restart the extension to pick up the new config:

```bash
ssh brightsign@192.168.0.165 '/var/volatile/bsext/ext_npu_anomaly/bsext_init restart'
```

> **Note**: `/storage/sd/` is the XT5's SD card, writable over SSH.
> The override persists across reboots and extension re-installs (the squashfs
> is read-only; SD card contents are preserved).

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

- Train/choose MobileNetV2 (often pretrained) on host.
- Export to ONNX (or use pretrained ONNX Model Zoo version).
- Convert + quantize to RKNN with a representative calibration set.
- Validate output tensor shape and accuracy on representative wall images.

---

## Architecture Overview

### Anomaly Detection Pipeline

```ini
Camera Input → ROI Extraction → Embedding Extraction → Reference Matching → Anomaly Detection
     ↓              ↓                    ↓                      ↓                    ↓
  GStreamer    OpenCV/RGA         MobileNetV2 (NPU)      Similarity Compare    Event Publisher
```

### MobileNetV2 Model (RKNN-Validated)

**Why MobileNetV2?**

- **Embedding Extraction**: Generates 1280-dim feature vectors per TV ROI
- **NPU Validated**: Tested in RKNN Model Zoo with documented performance
- **Robust**: Handles camera exposure, reflections, and minor distortions
- **Content-Agnostic**: Works with any video content (no face/object detection needed)
- **Efficient**: Lightweight architecture ideal for multi-TV monitoring

**Model Configuration:**

- Architecture: MobileNetV2 (ONNX Model Zoo)
- Input: 224×224×3 RGB (rectified ROI)
- Output: 1280-dimensional embedding vector (global average pooling)
- Current compiled output: 1000-dim classification layer (needs fixing)
- Quantization: INT8 for NPU acceleration
- Inference: ~10ms per ROI on RK3588

**Hardware Test Results (XT5):**

- ✅ Model loads: 3.97 MB
- ✅ NPU inference working correctly
- ✅ Output: 1000 dimensions (classification layer)
- ✅ Raw embeddings show good variation (-5.45 to 1.71 range)
- ✅ Correct classes detected (bell classes 494, 469, 442 in top-5)
- ⚠️ Softmax probabilities flattened due to INT8 quantization (doesn't affect anomaly detection)
- ⚠️ Need to extract 1280-dim embedding layer for optimal performance

See [MOBILENETV2_APPROACH.md](MOBILENETV2_APPROACH.md) for complete model pipeline details.

### Key Components (To Be Implemented)

1. **Input Module**

   - Camera capture (USB, RTSP)
   - ROI extraction from video wall displays
   - Frame preprocessing with RGA acceleration

2. **Model Module**

   - **MobileNetV2 embedding extraction** (primary model)
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
- **XS6/XD6/HD6** (RK3576) - Latest generation NPU

### Hardware Acceleration

- **NPU** - MobileNetV2 embedding inference (RKNN)
- **RGA** - 2D graphics acceleration (ROI warping, format conversion)
- **MPP** - Video decode acceleration (if using RTSP)
- **GStreamer** - Hardware-accelerated video pipeline

