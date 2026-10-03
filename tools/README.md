# Cerberus tools

Host-side helpers. Nothing here runs on the player.

## zone-picker — draw the ROI boxes (one per screen)

A small static web page for defining the `roi` rectangles the model watches: you
drag a box over each screen in the camera's field of view, and it emits the
cerberus `roi` config block (rect mode, `tv1..tvN`, absolute camera pixels).

Files:

- `zone-picker.html` — the page (a `<canvas>` over the camera image).
- `zone-picker.logic.js` — the pure ordering/scaling/clamping logic (unit-tested).
- `zone-picker.test.js` — node test for the logic: `node tools/zone-picker.test.js`.
- `zone-picker.sh` — launcher that serves the page and opens it pointed at a player.

### Use it

The camera image comes from the extension's **image-stream-server** (`/image`,
default port **20200**). The player must be running the extension so that
endpoint is up.

```sh
tools/zone-picker.sh --player <player-ip>          # live view, port 20200
tools/zone-picker.sh --player <player-ip> --once   # single snapshot, no refresh
tools/zone-picker.sh --url http://<player>:20200/image
```

Then, in the browser:

1. Drag a rectangle over each screen. Boxes are auto-numbered `tv1..tvN`
   top&rarr;bottom, left&rarr;right (matching `src/wvm/roi.cpp`). Delete any box
   from the list; "Clear all" resets.
2. Keep every box **fully inside the frame** — the capture crop drops
   out-of-bounds ROIs (the page flags/clamps them).
3. Get the config out, either way:
   - **Copy roi block** → paste over the `"roi"` key in your config, or
   - load your current `config.json` (the file input) and **Download config.json**
     to get a complete file with the new `roi` merged in.

### Apply on the player

ROIs are read at startup; there is no live reload. Put the config at the SD
override path and restart the extension:

```sh
scp config.json brightsign@<player>:/storage/sd/configs/config.json
# then, in the player's root shell:
/var/volatile/bsext/ext_npu_anomaly/bsext_init restart
```

The coordinates are **absolute pixels in the capture frame**, so they line up as
long as the capture resolution matches the image you drew on (the page shows the
detected frame size). No normalization.

### Notes

- No camera up yet? Load a saved frame with the file picker instead of a URL.
- The live image already shows the current ROI boxes/labels (it's the annotated
  `/tmp/output.jpg`), so you can see existing zones while redrawing.
- One axis-aligned rectangle per screen; rotated/quad zones are not supported
  (the engine's `RoiRect` is `x/y/w/h`).

## Python helpers

- `create_calibration_dataset.py`, `create_synthetic_calibration.py` — build INT8
  calibration image sets for model compilation.
- `inspect_onnx_simple.py`, `extract_embedding_layer.py` — ONNX inspection.
