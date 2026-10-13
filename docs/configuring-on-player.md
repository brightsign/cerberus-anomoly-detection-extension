---
title: "Configuring the NPU Anomaly-Detection Extension on a Player"
author: "BrightSign — Cerberus NPU Anomaly Detection"
date: "2026-10-06"
---

# Overview

All runtime behaviour is driven by a **single JSON file on the SD card**:

```
/storage/sd/configs/config.json
```

This one file holds the camera device, the model path, the per-screen
regions of interest (ROIs), the TV-off / anomaly thresholds, and the MQTT
and logging settings. There is no second "base" config and nothing in the
registry duplicates it — edit this file (or use the on-player editor) and
restart the extension.

## How the file gets there

On first boot the extension **seeds** the SD file from the packaged default
if it does not already exist. The lookup order the extension uses is:

1. `$BSEXT_CONFIG` (environment override — rarely used)
2. `/storage/sd/configs/config.json`  ← **the editable copy; edit this**
3. the packaged read-only default inside the extension (used only to seed #2)

Because the extension mount is read-only and volatile, your edits must live
on the SD card (#2), never inside the extension tree.

# Two ways to configure

## 1. The on-player web editor (easiest)

The extension runs a small **config server on port 20300**. Point a browser
on the same network at:

```
http://<player-ip>:20300/
```

You get a live editor that:

- loads the current `config.json` from the player,
- lets you **draw the ROI box over each screen** on the live camera image
  (boxes are auto-numbered `tv1`, `tv2`, ... top-to-bottom, left-to-right),
- lets you edit the **TV-off detection tuning** and **camera exposure**
  fields, each with an info (i) tooltip, and
- offers **Save** (write the file) or **Save & Restart** (write it and
  restart the extension so the changes take effect immediately).

The camera image shown in the editor is the live annotated frame, so you can
see existing zones while you redraw them.

## 2. Edit the JSON directly

Copy the file down, edit it, copy it back, and restart:

```sh
# from your workstation (brightsign user, scp works without a shell)
scp brightsign@<player-ip>:/storage/sd/configs/config.json .
#   ...edit config.json...
scp config.json brightsign@<player-ip>:/storage/sd/configs/config.json
```

Then apply it (see below). ROI coordinates are **absolute pixels in the
capture frame** (the `width` x `height` you set under `device`), so they line
up 1:1 with the camera image — no normalisation.

# Applying a change

A config change takes effect only when the extension restarts:

- **Web editor:** click **Save & Restart**.
- **By hand**, from the root Linux shell on the player:

  ```sh
  /var/volatile/bsext/ext_npu_anomaly/bsext_init restart
  ```

- Or simply **reboot** the player.

# Registry keys (optional)

Most configuration is in `config.json`. A few operational overrides live in
the BrightSign registry and are read at start:

| Section      | Key                                 | Purpose                                           |
|--------------|-------------------------------------|---------------------------------------------------|
| `extension`  | `bsext-anomaly-video-device`        | Force a camera path (e.g. `/dev/video1`); empty = auto-detect |
| `extension`  | `bsext-anomaly-disable-auto-start`  | `true` to stop the extension auto-starting at boot |
| `networking` | `bs-config-server-port`             | Change the editor port (default `20300`; `0` disables it) |
| `networking` | `bs-image-stream-server-port`       | Change the camera-image port (default `20200`)    |

Example (BrightSign shell): `registry write extension bsext-anomaly-video-device /dev/video1`

# A basic example

This is a minimal **single-screen** config derived from the one in use —
one USB camera, one TV zone, auto reference, MQTT to the local broker. Add
more screens by adding entries to `roi.tvs` (`tv2`, `tv3`, ...).

```json
{
  "device": {
    "camera_device": "usb_camera",
    "width": 1280,
    "height": 720,
    "fps": 30,
    "pixel_format": "YUYV"
  },
  "model": {
    "rknn_path": "model/mobilenetv2-embedding-rk3568.rknn",
    "input_w": 224,
    "input_h": 224,
    "embedding_dim": 1280
  },
  "reference": {
    "enabled": true,
    "ref_fps": 5,
    "mode": "auto",
    "search_window_seconds": 2.0,
    "auto_source": "tv_id",
    "ref_tv_id": "tv1",
    "auto_min_ref_seconds": 3.0,
    "auto_persist": true,
    "auto_persist_path": "/storage/sd/references/auto_ref_embeddings.f32",
    "auto_max_seconds": 900
  },
  "roi": {
    "mode": "rect",
    "tvs": [
      { "id": "tv1", "x": 160, "y": 90, "w": 960, "h": 540 }
    ]
  },
  "anomaly": {
    "enabled": true,
    "sample_fps": 5,
    "black_enter_mean": 120.0,
    "black_exit_mean": 140.0,
    "black_enter_var": 10000.0,
    "black_exit_var": 11000.0,
    "persist_black_ms": 500,
    "persist_black_recover_ms": 500,
    "persist_freeze_ms": 2000,
    "persist_mismatch_ms": 2500,
    "lag_threshold_s": 1.5,
    "stutter_window_ms": 3000,
    "similarity_min": 0.75,
    "camera_timeout_ms": 1500,
    "peer_enabled": false,
    "peer_similarity_min": 0.85,
    "persist_outlier_ms": 2500
  },
  "mqtt": {
    "enabled": true,
    "host": "127.0.0.1",
    "port": 1883,
    "topic": "videowall/events",
    "client_id": "wall-monitor"
  },
  "logging": {
    "level": "INFO",
    "path": "/tmp/videowall-monitor.log"
  }
}
```

## The fields you will touch most

- **`model.rknn_path`** — must match the player's SoC: `rk3568` (LS5),
  `rk3576` (XS6/XD6), `rk3588` (XT5). The packaged default ships a
  `@TARGET_SOC@` placeholder that is filled in at install; the SD copy
  should name the real SoC as above.
- **`roi.tvs`** — one rectangle per screen, in absolute capture pixels
  (`x`, `y`, `w`, `h`). Keep every box fully inside the `width` x `height`
  frame; out-of-bounds ROIs are dropped. Easiest to set with the web editor.
- **`reference.ref_tv_id`** — which screen is the "known good" reference the
  others are compared against (here `tv1`).
- **`anomaly.black_*` and `persist_black_*`** — the TV-off (black screen)
  detection thresholds and debounce. Raise the `persist_*` values if you see
  false flips; these are the fields the web editor's "TV-off tuning" section
  edits.

## Optional: manual camera exposure

If auto-exposure makes a still-off screen briefly read as ON when a
neighbouring screen turns off, pin the exposure by adding these to `device`:

```json
"auto_exposure": false,
"exposure_absolute": 250,
"gain": 0,
"auto_white_balance": false,
"white_balance_temperature": 4000
```

Leave them out (or `auto_exposure: true`) to let the camera manage exposure.
