# Changelog

All notable changes to this project are documented here. This project adheres to
[Semantic Versioning](https://semver.org/).

## v0.1.1 — 2026-10-06

### Fixed
- **Bundled uninstall script targeted the wrong extension.** `sh/uninstall.sh`
  still referenced the forked-from `npu_argus` name, so on a player it operated on
  `ext_npu_argus` paths/LVM volume instead of `ext_npu_anomaly` — failing to remove
  this extension and, on a player that also had Argus installed, removing the wrong
  one. All references corrected to `npu_anomaly`. The script ships inside the
  extension squashfs, so this requires a rebuild (and re-sign) to take effect.

### Docs
- Corrected the `brightsign-sdk-builder` repo/URL and clarified the one-time,
  shared-cache setup in `README.md` and `scripts/fetch-sdk.sh`.

## v0.1.0 — 2026-10-05

First tagged release of the BrightSign NPU anomaly-detection extension (XT5 /
RK3588, LS5 / RK3568, XS6/XD6 / RK3576).

### Build system
- **Shared SDK build cache.** The cross SDK, RKNN toolkit, and model-compile
  Docker image are provisioned once per box by the sibling `brightsign-sdk-builder`
  repo into a cache outside the repo (`../argus-build-cache`, override
  `ARGUS_CACHE_DIR`); this repo fetches from it instead of vendoring the SDK.
- **Makefile-driven build.** `make build` / `fetch-sdk` / `prep` / `build-models`
  / `build-engine` / `package` / `run-tests` / `clean`; retired `build-apps` and
  `compile-models`; removed the vendored SDK/OE/toolkit trees.
- **`make copy`** deploys the newest extension zip to a player (`.envrc`:
  `BS_PLAYER`/`BS_PASSWORD`; see `envrc-example`).
- Per-build **`install-on-player.sh`** + a self-contained LVM install script that
  stops the running extension (BusyBox-safe `killall`, lazy unmount), installs,
  and prunes old SD zips.

### Features
- **App-side camera autodetection.** The `usb_camera` sentinel resolves to a real
  `/dev/videoN` via V4L2 `VIDIOC_QUERYCAP`, using per-node `device_caps` so UVC
  metadata nodes are skipped (the real capture node is selected).
- **Single editable config on the SD card.** One `config.json` holds device,
  model, ROI zones, anomaly, and health settings; `bsext_init` seeds
  `/storage/sd/configs/config.json` on first boot.
- **On-player config editor (`config-server`, port 20300).** A browser tool to
  draw per-screen ROI zones over the live camera, edit TV-off detection tuning and
  camera exposure, and Save / Save & Restart — with info tooltips on each field.
- **Optional manual camera exposure** (`device.auto_exposure`, `exposure_absolute`,
  `gain`, white balance) to stop auto-exposure from flipping still-off screens to
  ON when another screen turns off.

### Requirements
- BrightSignOS 9.1 series; NPU-capable players (Series 5 XT5/LS5, Series 6
  XS6/XD6). Building requires the `brightsign-sdk-builder` cache + Docker.
