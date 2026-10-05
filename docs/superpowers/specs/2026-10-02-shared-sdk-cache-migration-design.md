# Design: Migrate cerberus to the shared SDK build-cache pattern

Date: 2026-10-02
Status: Draft for review

## 1. Purpose and intent

Cerberus currently vendors the entire BrightSignOS SDK toolchain and build
inputs inside the repo working tree (the installed `sdk/` ~1.6 GB, the
OpenEmbedded source tree `brightsign-oe/` ~40 GB, the RKNN `toolkit/` ~4.3 GB,
and the 285 MB cobra toolchain installer), and builds the SDK from source via
`scripts/runall.sh` step 0. Every BrightSign C++ extension that does this
re-downloads and rebuilds the same multi-hour, multi-gigabyte SDK.

`argus-rtsp-watcher` established a better pattern: a separate repo,
`brightsign-sdk-builder`, provisions the SDK + RKNN toolkit + model-compile
Docker image **once per build box** into a **shared cache that lives outside any
one repo**. Consumer repos read the SDK from that cache and compile their own
per-SoC models into it. This migration makes cerberus a consumer of that shared
cache.

Goal: cerberus no longer builds or vendors the SDK. It fetches the SDK from the
shared cache, compiles its models into the shared cache, cross-compiles its C++
app against the cache SDK, and packages the extension exactly as today.

This is the first C++/CMake extension to adopt the pattern (the sibling
`argus-audience-measurement-extension` has not been converted); the Go+C++
`argus-rtsp-watcher` is the reference.

## 2. The cache contract (what this repo may rely on)

`brightsign-sdk-builder`'s `make build` (run once per box) produces, in the
shared cache (`ARGUS_CACHE_DIR`, default `../argus-build-cache`):

| Path | Contents | Producer |
|------|----------|----------|
| `<cache>/sdk` | Cross SDK: `sysroots/aarch64-oe-linux`, `sysroots/x86_64-oesdk-linux`, `environment-setup-aarch64-oe-linux` (exports `OECORE_TARGET_SYSROOT`, `CC/CXX`), with OpenCV / GStreamer / RGA / turbojpeg / mosquitto / `librknnrt.so` / `usr/share/ext-bundle/ext-bundle-payload.tar.gz` in the sysroot | sdk-builder |
| `<cache>/toolkit` | `rknn-toolkit2` and `rknn_model_zoo` clones (v2.3.0) | sdk-builder |
| host `rknn_tk2` Docker image | model-conversion environment | sdk-builder |
| `<cache>/models` | per-SoC compiled `.rknn` | **this repo** (consumer) |
| `<cache>/bsoe` | OE build scratch | sdk-builder (internal; not ours) |

The SDK is relocated at install time to the cache's absolute path and is NOT
further relocatable. One correct install at the shared absolute path serves all
consumers.

## 3. Scope

### In scope
- Add the cache-resolution + fetch + prep + model-build scripts (ported from
  rtsp-watcher).
- Add a top-level `Makefile` as the single build entry point (per the
  `gherlein:makefile-builds` skill), retiring `build-apps`.
- Repoint `CMakeLists.txt` at the cache SDK / fetched RKNN / cache models.
- Repoint the `package` pipeline's model source at `<cache>/models`.
- Remove the vendored SDK-build files from version control and, after the cache
  is populated and the build is verified, delete the large on-disk blobs.

### Out of scope (explicit scope boundary)
- CMake remains the lib bundler. We do **not** port cerberus's in-CMake
  harvesting of OpenCV / GStreamer (ext-bundle) / mosquitto / RGA / turbojpeg
  out into `build-libs.sh` / `build-mosquitto.sh`. That harvesting already reads
  the SDK sysroot and works; it will simply read the cache SDK's sysroot. (This
  is the main structural difference from rtsp-watcher and is intentional, to
  avoid high-risk rewrite of a working bundler.)
- No changes to the application's runtime behavior, the extension layout on the
  player, `bsext_init`'s launch logic, or `sh/make-extension-lvm`.
- The Go `image-stream-server` optional target is left as-is.

## 4. Detailed changes

### 4.1 New: `scripts/lib/cache.sh`
Ported verbatim from `argus-rtsp-watcher/scripts/lib/cache.sh` (the consumer
variant that defines `MODELS_DIR`). Resolves `ARGUS_CACHE_DIR` (absolute, or
relative-to-repo-root) else `<repo>/../argus-build-cache`, normalized with
`realpath -m`. Exposes `CACHE_DIR SDK_DIR SDK_ENV TOOLKIT_DIR MODELS_DIR` both
as sourced vars and via `bash scripts/lib/cache.sh print <key>` for `$(shell …)`
in the Makefile. No cerberus-specific change.

### 4.2 New: `scripts/fetch-sdk.sh`
Ported from rtsp-watcher. Behavior:
- If `<cache>/sdk` is present and self-consistent (sysroot exists and
  `environment-setup` bakes `SDKTARGETSYSROOT=<cache>/sdk/...`), exit 0.
- Else install from an installer: `$1` / `SDK_INSTALLER`, else a
  `brightsign-x86_64-cobra-toolchain-*.sh` found in the cache dir, repo root, or
  CWD. Install with `-y -d <cache>/sdk` so OE relocation bakes the cache path.
- If no installer and no cache SDK: print the instruction to run
  `cd ../brightsign-sdk-builder && make build`, exit 1.
- After install, ensure `librknnrt.so` (pinned `RKNN_TAG=v2.3.0`) is present in
  the sysroot (wget fallback), matching rtsp-watcher.

Version decision (resolved): the SDK is the `brightsign-sdk-builder` 9.1-series
cache. The repo's local 9.0.189 cobra installer is retired (deleted from disk),
and the cache is populated by `cd ../brightsign-sdk-builder && make build`, not
from a local installer. `fetch-sdk.sh` keeps its `SDK_INSTALLER` fallback for
anyone holding a matching 9.1 installer, but the documented path is sdk-builder.

### 4.3 New: `scripts/prep.sh`
Ported from rtsp-watcher, pruned to the RKNN headers cerberus actually includes
plus `librknnrt.so`, fetched into `include/` (gitignored). This decouples
`CMakeLists.txt` from the `toolkit/` clone for the RKNN runtime/headers. The
exact header list is derived from cerberus's `#include` of RKNN API headers
(to be enumerated during implementation; today they resolve under
`toolkit/rknn-toolkit2/rknpu2/runtime/Linux/librknn_api/include` and the
model-zoo).

### 4.4 New: `scripts/build-models.sh`
Ported from rtsp-watcher's `build-models.sh`, with cerberus's model map:
- MobileNetV2 embedding — INT8 asymmetric, ImageNet mean/std, 1280-dim, from the
  `rknn_model_zoo` mobilenet example `mobilenetv2-12.onnx`, output
  `mobilenetv2-embedding-<soc>.rknn` (preserve current naming consumed by
  `bsext_init`/CMake).
- YOLOX-S — from the yolox example, output `yolox_s_<soc>.rknn`.

Runs the `rknn_tk2` image against `<cache>/toolkit`, writes
`<cache>/models/<SOC>/*.rknn` (+ `mobilenetv2_config.json` as today). Keeps the
per-SoC loop and `models_present()` idempotency. The INT8 vs fp handling and the
synthetic calibration dataset logic are carried over from the current
`compile-models` (lines ~266-416) so model numerics are unchanged.

### 4.5 New: top-level `Makefile`
Built with the `gherlein:makefile-builds` skill. Entry point for all builds.
`ARGUS_CACHE_DIR` is forwarded to `$(shell …)` and exported for the scripts, as
in rtsp-watcher. Targets:

- default (no arg): print the target list.
- `fetch-sdk` — `bash scripts/fetch-sdk.sh $(SDK_INSTALLER)`.
- `prep` — `bash scripts/prep.sh`.
- `build-models` — `bash scripts/build-models.sh $(MODELS_DIR) $(SOC_LIST)`.
- `build-engine` (or `build-apps`-equivalent) — for each SoC: `source $(SDK_ENV)
  && cmake … -DOECORE_TARGET_SYSROOT=$$OECORE_TARGET_SYSROOT -DTARGET_SOC=<soc>
  -DCMAKE_BUILD_TYPE=Release && make -j && make install`. Depends on
  `fetch-sdk prep`.
- `package` — depends on build + models; runs the existing `package` pipeline.
- `build` — `fetch-sdk prep build-models build-engine package` (full build).
- `run-tests` — host unit test(s): compile & run `test/test_camera_autodetect.cpp`
  (and any future engine host tests). Does not require the cross SDK.
- `test` — alias for `run-tests`.
- `clean` — remove `build_*/ install/ staging/ *.zip` and `include/` prep
  artifacts.
- `cache-info` / `cache-clean` — show / remove the shared cache (cache-clean
  warns it affects all projects).

Build-dir naming is standardized to `build_<soc>` (`build_rk3588` /
`build_rk3568` / `build_rk3576`), fixing the current `build-apps` inconsistency
where `Firebird` produced `build_firebird` while the rest of the tree uses
`build_rk3576`. (Confirm this rename does not break `package`'s build-dir
lookups; `package` reads `build_<platform>` today and will be updated in lock-
step.)

### 4.6 Changes: `CMakeLists.txt`
- RKNN: `RKNN_API_PATH` / `RKNN_RT_LIB` move from `${CMAKE_SOURCE_DIR}/toolkit/…`
  to the `include/`-fetched runtime (prep.sh) — mirroring rtsp-watcher's
  `RKNN_RT_LIB=${CMAKE_SOURCE_DIR}/include/librknnrt.so`. Headers likewise from
  `include/`.
- Models: remove the dead `copy_models` CMake target that reads
  `toolkit/mobilenetv2-embedding-${SOC}.rknn` at the toolkit root (a path
  nothing writes). Model staging is handled by the `package` pipeline from
  `<cache>/models`.
- Mosquitto: drop the `brightsign-oe/build/downloads/` tarball search path
  (that tree is being removed); keep the network download fallback and the
  sysroot client-lib harvest.
- Unchanged: `OECORE_TARGET_SYSROOT` usage, GStreamer ext-bundle extraction,
  OpenCV/TBB/RGA/turbojpeg glob harvest — now sourced from the cache SDK
  sysroot.
- Cleanup: collapse the two conflicting `CMAKE_INSTALL_PREFIX` definitions and
  the redundant double `install(TARGETS anomaly_detection …)` the inventory
  flagged, so install goes to `install/${SOC_DIR}` only.

### 4.7 Changes: `package` pipeline
- `copy_models` reads `<cache>/models/<SOC>` (via `cache.sh`) instead of
  `install/<SOC>/model`. All other staging (`copy_binaries`, `copy_configs`,
  `generate_manifest`, `copy_extension_scripts`) and both outputs (dev zip +
  `sh/make-extension-lvm` LVM extension zip) are unchanged.
- Update build-dir lookups if 4.5's `build_<soc>` rename lands.

### 4.8 Removals
Tracked files removed with `git rm`:
- `build-apps` (replaced by the Makefile).
- `compile-models` (replaced by `scripts/build-models.sh`).
- `Dockerfile` (the OE `bsoe-build` image — now owned by sdk-builder).
- `bsoe-recipes/` (the Yocto layer — now owned by sdk-builder).
- `scripts/runall.sh` — removed entirely (resolved). The Makefile is the sole
  orchestrator; SDK provisioning is sdk-builder's job.
- SDK-from-source helpers (resolved: remove, after confirming no non-OE caller):
  `scripts/clean_build.sh`, `scripts/install-gstreamer-plugins.sh`,
  `scripts/validate_bbappend.sh`.

On-disk blobs (currently gitignored, not tracked) — deleted **after** the cache
is populated and the full build is verified green:
- `sdk/` (~1.6 GB), `brightsign-oe/` (~40 GB), `toolkit/` (~4.3 GB).
- The local `brightsign-x86_64-cobra-toolchain-9.0.189.sh` (9.0 installer) is
  retired as part of the move to the 9.1 cache; deleted from disk (gitignored
  regardless).

### 4.9 `.gitignore`
Mirror rtsp-watcher: keep `build_*/ install/ sdk/ toolkit/ brightsign-oe/`
(already present), add `/staging/`, `*.rknn`, and the prep-fetched RKNN headers
/ `librknnrt.so` under `include/`.

## 5. Verification plan

1. `cache.sh` resolves expected paths (`print cache|sdk|sdk-env|toolkit|models`),
   and honors `ARGUS_CACHE_DIR`.
2. `<cache>/sdk` (9.1) is populated by `cd ../brightsign-sdk-builder && make
   build`; `make fetch-sdk` then detects it as self-consistent. (sdk-builder's
   `make build` is a multi-hour, ~25 GB first run; the cache currently holds only
   `<cache>/bsoe`, so this must complete before end-to-end verification.)
3. `make run-tests` — host `test_camera_autodetect` passes.
4. `make build-engine` cross-compiles against the cache SDK for at least RK3588;
   confirm the binary links and the new objects are present.
5. `make build-models` — validated against `<cache>/toolkit` + `rknn_tk2` if
   present; otherwise documented as requiring sdk-builder (Docker, ~10 GB).
6. `make package` — produces the dev zip and the LVM extension zip; staging
   layout matches the current `package` output (clean rebuild, since existing
   `install/` trees contain stale libs per the inventory).
7. Only after 1-6 are green: delete the on-disk `sdk/ brightsign-oe/ toolkit/`.

Prerequisite: the shared cache is currently unpopulated (only `<cache>/bsoe`
exists). Step 2 populates the SDK from the local installer; `<cache>/toolkit`
and the `rknn_tk2` image require `brightsign-sdk-builder`'s `make build` (or are
reused if already produced by another consumer).

## 6. Risks and notes (from the inventory)

- Existing `install/` trees contain **stale** libs (libperl.so, libav*,
  libasound, libdrm*, python3 `.pc`) not produced by the current CMake — a clean
  rebuild is required to establish the true bundle manifest; do not treat the
  current `install/` as ground truth.
- SDK is non-relocatable; never copy an SDK relocated for a different path into
  the cache — always install with `-d <cache>/sdk`.
- `build-apps Firebird` → `build_firebird` vs the rest using `build_rk3576`: the
  Makefile standardizes on `build_<soc>`; `package` updated in lock-step.
- Model naming (`mobilenetv2-embedding-<soc>.rknn`, `yolox_s_<soc>.rknn`) must be
  preserved — `bsext_init` and the app depend on it.
- SDK version: standardized on the sdk-builder **9.1** cache. The 9.0.189 local
  installer is retired. If the 9.1 sysroot changes library sonames vs 9.0, the
  CMake glob harvests absorb it, but confirm OpenCV/GStreamer/mosquitto versions
  during the first cross-build.

## 7. Resolved decisions

1. OS/SDK version: **sdk-builder 9.1 cache** (local 9.0.189 installer retired).
2. `scripts/runall.sh`: **removed entirely**; the Makefile is the orchestrator.
3. `clean_build.sh` / `install-gstreamer-plugins.sh` / `validate_bbappend.sh`:
   **removed**, after confirming no non-OE caller during implementation.
