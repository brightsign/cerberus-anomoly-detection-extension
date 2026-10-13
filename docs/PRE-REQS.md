# Prerequisites: SDK, Shared Build Cache, and Repo Configuration

The cross-compile SDK (hours to build, tens of GB) and the compiled NPU models
do **not** live in this repo. They live in a **shared build cache outside the
repo**, built once per box and reused across our NPU extensions. This repo is a
*consumer*: it resolves the cache, verifies it, and cross-compiles against it.

> **TL;DR (cache already built on this box):**
> `cp cache.env.example cache.env`, set `BS_CACHE_DIR`, then `make cache-info`
> and `make build`.

```
brightsign-sdk-builder  --build-->  shared cache (SDK + toolkit + models)
                                        ^
   this repo  --cache.env/BS_CACHE_DIR--+--  make build  -->  extension zip
```

## Supported targets

| Player | SoC | `TARGET_SOC` | Build dir | Install dir | Model suffix |
|---|---|---|---|---|---|
| XT5 (XT1145, XT2145) | RK3588 | `rk3588` | `build_rk3588` | `install/RK3588` | `rk3588` |
| XS6 / XD6 (Series 6) | RK3576 | `rk3576` | `build_rk3576` | `install/RK3576` | `rk3576` |
| LS5 (LS445) | RK3568 | `rk3568` | `build_rk3568` | `install/RK3568` | `rk3568` |

RK3576 (Series 6) is flagged **experimental** in the manifest.

## Prerequisites (host)

- x86_64 Linux (Ubuntu/Debian).
- **Docker** (for the `rknn_tk2` model-compile image) and ~25 GB free disk.
- Host tools: `make`, `cmake`, `g++`, `git`, `curl`/`wget`, `jq`, `mksquashfs`
  (`squashfs-tools`), `zip`, `unzip`, Go (for the bundled Go services), Node
  (for the zone-picker JS test).
- Network reach to the BrightSign SDK source, GitHub (RKNN toolkit + runtime),
  and `mosquitto.org` (or a pre-seeded tarball — see Phase 1 step 4).

## Phase 1 — Build the SDK and populate the cache (one-time per box)

You only do this **once** per build box; the cache is shared, so many of our
example extensions reuse it with no per-repo SDK rebuild.

> **Automatic:** if the configured cache has no SDK, `make build` / `make fetch-sdk`
> build it for you — `scripts/fetch-sdk.sh` locates (or clones) `brightsign-sdk-builder`
> and runs `make -C <builder> sdk ARGUS_CACHE_DIR=<your cache>`. This is the
> multi-hour, ~25 GB job below, just triggered on demand. Disable it with
> `BS_SDK_AUTOBUILD=0` (you then get manual instructions), or point `SDK_BUILDER_DIR`
> at an existing checkout. The RKNN toolkit, `rknn_tk2` image, and models are
> provisioned separately by `make build-models` (step 3), so a bare cache still
> builds in one `make build`.

### 1. Build the cross SDK + toolkit + model image into the cache

```bash
git clone https://github.com/brightsign/brightsign-sdk-builder.git
cd brightsign-sdk-builder && make build
```

This builds the BrightSign aarch64 cross SDK, the RKNN toolkit, and the
`rknn_tk2` Docker image into the shared cache (default layout:
`<cache>/sdk`, `<cache>/toolkit`, `<cache>/models`). The SDK sysroot it produces
must include the engine's native dependencies: **OpenCV, GStreamer, GLib, RGA,
TurboJPEG, and the mosquitto client libraries** (`make sdk-libs` in this repo
gates on their presence — see Phase 2).

### 2. librknnrt runtime

`make fetch-sdk` / `make prep` in this repo fetch `librknnrt.so` (Rockchip NPU
runtime, pinned `v2.3.0`) if the cache does not already carry it.

### 3. Compile the NPU models into the cache

From this repo (needs Docker + the `rknn_tk2` image + ~10 GB):

```bash
make build-models            # all SoCs; or: make build-models SOC_LIST=RK3588
```

Produces, per SoC, under `<cache>/models/<SOC>/`:
`mobilenetv2-embedding-<soc>.rknn` (required), `yolox_s_<soc>.rknn` (optional
startup TV-rectangle detector), and `mobilenetv2_config.json`.

### 4. mosquitto source tarball (offline builds)

The engine's CMake builds the **mosquitto broker 2.0.18** from source and looks
in `downloads/` before hitting `mosquitto.org`. On a build box without outbound
access to mosquitto.org, seed the tarball into the cache once:

```bash
mkdir -p <cache>/downloads
curl -L -o <cache>/downloads/mosquitto-2.0.18.tar.gz \
  https://mosquitto.org/files/source/mosquitto-2.0.18.tar.gz
```

`make build` stages `<cache>/downloads/*.tar.gz` into the repo's `downloads/`
automatically.

## Phase 2 — Configure this repo

```bash
cp cache.env.example cache.env
# edit cache.env: BS_CACHE_DIR := /abs/path/to/your/shared/build-cache

make cache-info      # confirms the resolved cache, SDK, toolkit, models
make fetch-sdk       # verifies the SDK is present and baked for this path
make sdk-libs        # gates: SDK sysroot ships OpenCV/GStreamer/... (actionable if not)
```

If `BS_CACHE_DIR` is unset, the build auto-discovers a sibling
`../argus-all/argus-build-cache` holding a built SDK, else falls back to
`../argus-build-cache`.

## Build, test, package

```bash
make build           # fetch-sdk -> sdk-libs -> prep -> build-models -> cross-compile -> package
make run-tests       # host unit tests (no SDK/device/docker)
make copy            # scp newest ext zip + install-on-player.sh to a player (BS_PLAYER/BS_PASSWORD in .envrc)
```

`make build` is one-shot once the cache is fully populated. Intermediate
targets: `fetch-sdk`, `sdk-libs`, `prep`, `build-models`, `build-engine`,
`package`.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `cannot execute: required file not found` from the cross compiler | SDK relocated for a different absolute path | `rm -rf <cache>/sdk` and re-run `make fetch-sdk` so OE relocation bakes this cache's path |
| `make sdk-libs` lists MISSING OpenCV/GStreamer/... | SDK sysroot lacks those packages | Rebuild the SDK via `brightsign-sdk-builder` so the sysroot includes them (do **not** hand-build them here) |
| mosquitto download fails during build | No reach to mosquitto.org | Seed `<cache>/downloads/mosquitto-2.0.18.tar.gz` (Phase 1 step 4) |
| `build-models` tries Docker and fails | Docker missing or `rknn_tk2` image absent | Install Docker; the script rebuilds the image from the toolkit clone |
| `make cache-info` shows `sdk ... [absent]` | `BS_CACHE_DIR` wrong, or cache not built | Fix `cache.env`, or run Phase 1 |

## Why a shared cache?

The SDK and compiled models are expensive to produce and identical across
projects. Keeping them in a cache outside any one repo means they are built once
per box and reused by every NPU extension — no vendoring, no per-repo SDK
rebuild, and a reproducible path the OE installer can bake in.
