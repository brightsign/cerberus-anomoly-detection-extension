# Cerberus tools

Host-side helpers. (The zone-picker page itself now runs on the player, served by
`srv/config-server`; `tools/zone-picker.sh` just opens it.)

## zone-picker — draw the ROI boxes (one per screen)

Draw a box over each screen in the camera's field of view to define the `roi`
rectangles the model watches (rect mode, `tv1..tvN`, absolute camera pixels). It
runs **on the player**: the `config-server` (started by `bsext_init`) serves the
page and reads/writes the live config at `/storage/sd/configs/config.json`.

The page and its logic live in the server so the binary can embed them:

- `srv/config-server/web/zone-picker.html` — the page (`<canvas>` over the camera image).
- `srv/config-server/web/zone-picker.logic.js` — pure ordering/scaling/clamping (unit-tested).
- `srv/config-server/web/zone-picker.test.js` — `node .../zone-picker.test.js` (run via `make run-tests`).
- `srv/config-server/main.go` — the on-player HTTP server (embeds the two web files).
- `tools/zone-picker.sh` — opens the on-player page in your browser.

### Use it (on-player)

The extension must be running (the config-server listens on port **20300** by
default; the camera image comes from the image-stream-server on **20200**).

```sh
tools/zone-picker.sh --player <player-ip>     # opens http://<player>:20300/
```

In the browser:

1. The page **loads the live config** from the player; existing zones appear as
   editable boxes.
2. Drag a rectangle over each screen; delete/redraw as needed. Boxes are
   auto-numbered `tv1..tvN` top&rarr;bottom, left&rarr;right (matching
   `src/wvm/roi.cpp`). "Clear all" resets.
3. Keep every box **fully inside the frame** — the capture crop drops
   out-of-bounds ROIs (the page flags/clamps them).
4. **Save to player** writes the live config; **Save & Restart** writes it and
   restarts the extension so the zones take effect (ROIs are read at startup).

Coordinates are **absolute pixels in the capture frame**, so they line up with
the camera image (the page shows the detected frame size). No normalization.

### Offline (no running player)

Open `srv/config-server/web/zone-picker.html` directly in a browser. There is no
server, so the page falls back to the file-drop workflow: **load config.json**
(a copy you scp'd down), draw, **Download config.json**, then copy it to
`/storage/sd/configs/config.json` and `bsext_init restart`.

### Notes

- One axis-aligned rectangle per screen; rotated/quad zones are not supported
  (the engine's `RoiRect` is `x/y/w/h`).
- The camera image already shows the current ROI boxes/labels (it is the
  annotated `/tmp/output.jpg`), so you see existing zones while redrawing.

## Python helpers

- `create_calibration_dataset.py`, `create_synthetic_calibration.py` — build INT8
  calibration image sets for model compilation.
- `inspect_onnx_simple.py`, `extract_embedding_layer.py` — ONNX inspection.
