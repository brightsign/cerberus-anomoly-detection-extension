# Shared SDK Build-Cache Migration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make cerberus a consumer of the shared `brightsign-sdk-builder` cache — stop vendoring/building the SDK, fetch it from the cache, compile models into the cache, cross-compile and package as today, driven by a new Makefile.

**Architecture:** Port rtsp-watcher's cache-resolution + fetch + prep + model-build scripts, add a Makefile as the sole build entry point that sources the cache SDK and drives the existing CMake build and `package` pipeline. CMake remains the lib bundler (reads the cache SDK sysroot). Remove the vendored SDK-build files and the 46 GB of on-disk blobs after verification.

**Tech Stack:** GNU Make, Bash, CMake, OpenEmbedded cross SDK (aarch64), Docker (rknn_tk2) for model conversion, RKNN toolkit v2.3.0.

**Spec:** `docs/superpowers/specs/2026-10-02-shared-sdk-cache-migration-design.md`

## Global Constraints

- Shared cache location: `ARGUS_CACHE_DIR` (absolute, or relative-to-repo-root), default `../argus-build-cache`, normalized with `realpath -m`.
- Cache paths (verbatim): `<cache>/sdk`, `<cache>/sdk/environment-setup-aarch64-oe-linux`, `<cache>/toolkit`, `<cache>/models`.
- SDK: the `brightsign-sdk-builder` **9.1** cache. The local `brightsign-x86_64-cobra-toolchain-9.0.189.sh` is retired. SDK is non-relocatable — install only with `-d <cache>/sdk`.
- Platform -> SOC -> install dir: `XT5 -> rk3588 -> RK3588`, `LS5 -> rk3568 -> RK3568`, `Firebird -> rk3576 -> RK3576`. Build dirs standardized to `build_<soc>` (`build_rk3588`, `build_rk3568`, `build_rk3576`).
- `SOC_LIST` default: `RK3588 RK3576 RK3568`.
- `RKNN_TAG` default: `v2.3.0`.
- Compiled model filenames preserved exactly: `mobilenetv2-embedding-<soc>.rknn`, `yolox_s_<soc>.rknn` (and `mobilenetv2_config.json`). `bsext_init` and the app depend on these names.
- Build and test ONLY through the Makefile. Required targets: `build`, `test`, `clean`, `run-tests`. Default target (no arg) prints the target list.
- Makefile authored via the `gherlein:makefile-builds` skill.
- No emoji characters in code. Comments explain WHY, not WHAT. Handle errors explicitly; no silent fallbacks.

## Review Focus

- **Relative vs absolute `ARGUS_CACHE_DIR`** — a relative override must resolve against the repo root (not CWD) and normalize away `..`; an absolute one is used as-is. Pinned in Task 1.
- **Stale/wrong-path SDK already in `<cache>/sdk`** — `fetch-sdk` must detect that the installed SDK's `environment-setup` does not bake this cache's path and error loudly, never silently use a broken SDK. Pinned in Task 2.
- **Cache not yet built** — any Make target needing the SDK must fail with the actionable "run `cd ../brightsign-sdk-builder && make build`" message, not a cryptic cmake/compiler error. Pinned in Task 2 and Task 7.
- **`build-models` idempotency / partial cache** — re-running must skip a SOC whose models already exist and must not leave a half-written model on failure. Pinned in Task 5.
- **Model filename contract** — a SOC's staged `model/` must contain exactly `mobilenetv2-embedding-<soc>.rknn` + `yolox_s_<soc>.rknn`; a rename breaks `bsext_init`. Pinned in Task 5 and Task 7.

---

## File Structure

New:
- `scripts/lib/cache.sh` — cache path resolver (single source of truth).
- `scripts/fetch-sdk.sh` — ensure/validate the cache SDK.
- `scripts/prep.sh` — fetch RKNN headers + `librknnrt.so` into `include/`.
- `scripts/build-models.sh` — compile per-SOC `.rknn` into `<cache>/models`.
- `Makefile` — build entry point.
- `test/cache_sh_test.sh` — host test for `cache.sh` resolution.
- `test/fetch_sdk_test.sh` — host test for `fetch-sdk.sh` guard rails.

Modified:
- `CMakeLists.txt` — RKNN path -> `include/`, drop dead `copy_models` target, drop `brightsign-oe` mosquitto search path, collapse duplicate install-prefix/targets.
- `package` — read models from `<cache>/models/<SOC>`; build-dir lookups to `build_<soc>`.
- `.gitignore` — add `/staging/`, `*.rknn`, prep-fetched `include/` RKNN artifacts.

Removed (git rm): `build-apps`, `compile-models`, `Dockerfile`, `bsoe-recipes/`, `scripts/runall.sh`, `scripts/clean_build.sh`, `scripts/install-gstreamer-plugins.sh`, `scripts/validate_bbappend.sh`.

Deleted on disk after verification: `sdk/`, `brightsign-oe/`, `toolkit/`, `brightsign-x86_64-cobra-toolchain-9.0.189.sh`.

---

### Task 1: Cache path resolver (`scripts/lib/cache.sh`)

**Files:**
- Create: `scripts/lib/cache.sh`
- Test: `test/cache_sh_test.sh`

**Interfaces:**
- Produces: `bash scripts/lib/cache.sh print <key>` prints one path for `key` in `cache|sdk|sdk-env|toolkit|models`. Sourcing it exports `CACHE_DIR SDK_DIR SDK_ENV TOOLKIT_DIR MODELS_DIR`. Resolution: `ARGUS_CACHE_DIR` if set (absolute kept, relative joined to `REPO_ROOT`), else `<REPO_ROOT>/../argus-build-cache`, normalized `realpath -m`. `REPO_ROOT` defaults to two levels above this file.

- [ ] **Step 1: Write the failing test**

```bash
# test/cache_sh_test.sh
#!/usr/bin/env bash
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIB="$REPO/scripts/lib/cache.sh"
fail=0
check() { # label actual expected
  if [ "$2" = "$3" ]; then echo "  ok   $1"; else echo "  FAIL $1: got '$2' want '$3'"; fail=1; fi
}

# Default: parallel to repo, normalized absolute.
exp_default="$(realpath -m "$REPO/../argus-build-cache")"
check "default cache"   "$(bash "$LIB" print cache)" "$exp_default"
check "default sdk"     "$(bash "$LIB" print sdk)"     "$exp_default/sdk"
check "default sdk-env" "$(bash "$LIB" print sdk-env)" "$exp_default/sdk/environment-setup-aarch64-oe-linux"
check "default toolkit" "$(bash "$LIB" print toolkit)" "$exp_default/toolkit"
check "default models"  "$(bash "$LIB" print models)"  "$exp_default/models"

# Absolute override is used verbatim.
check "abs override"    "$(ARGUS_CACHE_DIR=/tmp/xyzcache bash "$LIB" print cache)" "/tmp/xyzcache"

# Relative override resolves against REPO_ROOT and normalizes '..'.
check "rel override"    "$(ARGUS_CACHE_DIR=sub/../bc bash "$LIB" print cache)" "$(realpath -m "$REPO/bc")"

[ "$fail" = 0 ] && echo "ALL PASS" || { echo "FAILURES"; exit 1; }
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bash test/cache_sh_test.sh`
Expected: FAIL (cache.sh does not exist yet).

- [ ] **Step 3: Create `scripts/lib/cache.sh`**

Port verbatim from `../argus-all/argus-rtsp-watcher/scripts/lib/cache.sh` (the consumer variant that defines `MODELS_DIR`). Content:

```bash
#!/usr/bin/env bash
# Single source of truth for the shared build cache location and the paths
# derived from it. The SDK, the RKNN toolkit clone, and the compiled models are
# expensive to produce and identical across projects, so they live in a cache
# OUTSIDE any one repo, built once and reused.
#
# Override with ARGUS_CACHE_DIR (absolute recommended; a relative value is
# resolved against the repo root). Default sits parallel to the repo.
#   source scripts/lib/cache.sh                 # sets CACHE_DIR SDK_DIR SDK_ENV TOOLKIT_DIR MODELS_DIR
#   bash scripts/lib/cache.sh print <key>       # cache | sdk | sdk-env | toolkit | models

_cache_lib_dir="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "${_cache_lib_dir}/../.." && pwd)}"

if [ -n "${ARGUS_CACHE_DIR:-}" ]; then
    case "${ARGUS_CACHE_DIR}" in
        /*) CACHE_DIR="${ARGUS_CACHE_DIR}" ;;
        *)  CACHE_DIR="${REPO_ROOT}/${ARGUS_CACHE_DIR}" ;;
    esac
else
    CACHE_DIR="${REPO_ROOT}/../argus-build-cache"
fi
CACHE_DIR="$(realpath -m "${CACHE_DIR}" 2>/dev/null || echo "${CACHE_DIR}")"

SDK_DIR="${CACHE_DIR}/sdk"
SDK_ENV="${SDK_DIR}/environment-setup-aarch64-oe-linux"
TOOLKIT_DIR="${CACHE_DIR}/toolkit"
MODELS_DIR="${CACHE_DIR}/models"

export CACHE_DIR SDK_DIR SDK_ENV TOOLKIT_DIR MODELS_DIR

if [ "${1:-}" = "print" ]; then
    case "${2:-cache}" in
        cache)   echo "${CACHE_DIR}" ;;
        sdk)     echo "${SDK_DIR}" ;;
        sdk-env) echo "${SDK_ENV}" ;;
        toolkit) echo "${TOOLKIT_DIR}" ;;
        models)  echo "${MODELS_DIR}" ;;
        *)       echo "unknown cache key: ${2}" >&2; exit 1 ;;
    esac
fi
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `bash test/cache_sh_test.sh`
Expected: `ALL PASS`.

- [ ] **Step 5: Commit**

```bash
git add scripts/lib/cache.sh test/cache_sh_test.sh
git commit -m "build: add shared build-cache path resolver (cache.sh)"
```

---

### Task 2: SDK fetch/validate guard (`scripts/fetch-sdk.sh`)

**Files:**
- Create: `scripts/fetch-sdk.sh`
- Test: `test/fetch_sdk_test.sh`

**Interfaces:**
- Consumes: `scripts/lib/cache.sh` (`SDK_DIR`, `SDK_ENV`, `CACHE_DIR`).
- Produces: `bash scripts/fetch-sdk.sh [installer]`. Exit 0 if `<cache>/sdk` is present and self-consistent; exit 1 with the sdk-builder instruction if absent and no installer; exit 1 if `<cache>/sdk` exists but is not self-consistent (stale path).

- [ ] **Step 1: Write the failing test**

```bash
# test/fetch_sdk_test.sh
#!/usr/bin/env bash
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SH="$REPO/scripts/fetch-sdk.sh"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0

# Empty cache, no installer -> exit 1 and name brightsign-sdk-builder.
out="$(ARGUS_CACHE_DIR="$TMP/empty" bash "$SH" 2>&1)"; rc=$?
{ [ "$rc" = 1 ] && echo "$out" | grep -q "brightsign-sdk-builder"; } \
  && echo "  ok   empty-cache guidance" || { echo "  FAIL empty-cache: rc=$rc"; fail=1; }

# A cache/sdk that exists but whose env-setup does NOT bake this path -> error, exit 1.
mkdir -p "$TMP/stale/sdk/sysroots/aarch64-oe-linux"
echo "SDKTARGETSYSROOT=/some/other/path/sysroots/aarch64-oe-linux" > "$TMP/stale/sdk/environment-setup-aarch64-oe-linux"
out="$(ARGUS_CACHE_DIR="$TMP/stale" bash "$SH" 2>&1)"; rc=$?
{ [ "$rc" = 1 ] && echo "$out" | grep -qi "not .*self-consistent\|does not reference"; } \
  && echo "  ok   stale-sdk rejected" || { echo "  FAIL stale-sdk: rc=$rc out=$out"; fail=1; }

# A self-consistent cache/sdk -> exit 0.
mkdir -p "$TMP/good/sdk/sysroots/aarch64-oe-linux"
echo "SDKTARGETSYSROOT=$TMP/good/sdk/sysroots/aarch64-oe-linux" > "$TMP/good/sdk/environment-setup-aarch64-oe-linux"
out="$(ARGUS_CACHE_DIR="$TMP/good" bash "$SH" 2>&1)"; rc=$?
[ "$rc" = 0 ] && echo "  ok   good-sdk accepted" || { echo "  FAIL good-sdk: rc=$rc out=$out"; fail=1; }

[ "$fail" = 0 ] && echo "ALL PASS" || { echo "FAILURES"; exit 1; }
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bash test/fetch_sdk_test.sh`
Expected: FAIL (fetch-sdk.sh does not exist).

- [ ] **Step 3: Create `scripts/fetch-sdk.sh`**

Port from `../argus-all/argus-rtsp-watcher/scripts/fetch-sdk.sh`, keeping: `sdk_is_self_consistent()` (checks the sysroot dir exists AND `grep -q "SDKTARGETSYSROOT=${SDK_DIR}/sysroots/aarch64-oe-linux" "${SDK_ENV}"`); the "exists but not self-consistent -> error, remove and re-run" branch; the installer search (`$1`, cache dir, repo root, CWD) for `brightsign-x86_64-cobra-toolchain-*.sh`; install via `"${INSTALLER}" -y -d "${SDK_DIR}"`; the `librknnrt.so` (`RKNN_TAG=v2.3.0`) wget-into-sysroot step; and the no-installer heredoc that instructs `cd ../brightsign-sdk-builder && make build`. Source `scripts/lib/cache.sh` for `SDK_DIR`/`SDK_ENV`/`CACHE_DIR`. Do not hardcode the 9.0.189 installer name anywhere beyond the generic `*` glob.

- [ ] **Step 4: Run the test to verify it passes**

Run: `bash test/fetch_sdk_test.sh`
Expected: `ALL PASS`.

- [ ] **Step 5: Syntax-check and commit**

```bash
bash -n scripts/fetch-sdk.sh
git add scripts/fetch-sdk.sh test/fetch_sdk_test.sh
git commit -m "build: add fetch-sdk guard that validates the cache SDK"
```

---

### Task 3: Makefile skeleton (resolver wiring, help, host tests)

**Files:**
- Create: `Makefile`

**Interfaces:**
- Consumes: `scripts/lib/cache.sh`, the host unit tests.
- Produces: targets `help` (default), `cache-info`, `run-tests`, `test`, `fetch-sdk`, `prep`, `clean`, plus `CACHE_DIR/SDK_DIR/SDK_ENV/TOOLKIT_DIR/MODELS_DIR` Make vars and `SOC_LIST`. `build`, `build-models`, `build-engine`, `package` are added in Tasks 5 and 7.

Authored with the `gherlein:makefile-builds` skill. Use this structure:

```make
# ARGUS_CACHE_DIR is a Make variable when set on the command line, not an env var,
# so forward it explicitly to the $(shell ...) calls AND export it for the scripts.
CACHE_SH := ARGUS_CACHE_DIR='$(ARGUS_CACHE_DIR)' bash scripts/lib/cache.sh print
CACHE_DIR   := $(shell $(CACHE_SH) cache)
SDK_DIR     := $(shell $(CACHE_SH) sdk)
SDK_ENV     := $(shell $(CACHE_SH) sdk-env)
TOOLKIT_DIR := $(shell $(CACHE_SH) toolkit)
MODELS_DIR  := $(shell $(CACHE_SH) models)
export ARGUS_CACHE_DIR

SOC_LIST ?= RK3588 RK3576 RK3568
SDK_INSTALLER ?=

.DEFAULT_GOAL := help

help:                ## Print available targets
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | awk 'BEGIN{FS=":.*?## "}{printf "  %-14s %s\n", $$1, $$2}'

cache-info:          ## Show the resolved shared-cache location and presence
	@echo "cache   : $(CACHE_DIR)"
	@echo "sdk     : $(SDK_DIR)  [$(if $(wildcard $(SDK_ENV)),present,absent)]"
	@echo "toolkit : $(TOOLKIT_DIR)  [$(if $(wildcard $(TOOLKIT_DIR)),present,absent)]"
	@echo "models  : $(MODELS_DIR)  [$(if $(wildcard $(MODELS_DIR)),present,absent)]"

fetch-sdk:           ## Ensure the aarch64 cross SDK is present in the shared cache
	bash scripts/fetch-sdk.sh $(SDK_INSTALLER)

prep:                ## Fetch RKNN headers + runtime into include/ (needs network)
	bash scripts/prep.sh

run-tests:           ## Run host unit tests (no cross SDK needed)
	bash test/cache_sh_test.sh
	bash test/fetch_sdk_test.sh
	g++ -std=c++17 -Wall -Iinclude test/test_camera_autodetect.cpp src/wvm/camera_autodetect.cpp src/wvm/logger.cpp -o /tmp/test_camera_autodetect
	/tmp/test_camera_autodetect

test: run-tests      ## Alias for run-tests

clean:               ## Remove build artifacts (build_*/ install/ staging/ zips + prep headers)
	rm -rf build_* install staging *.zip
	bash scripts/prep.sh clean 2>/dev/null || true

.PHONY: help cache-info fetch-sdk prep run-tests test clean
```

- [ ] **Step 1: Write the Makefile** (content above).

- [ ] **Step 2: Verify the default target lists targets**

Run: `make`
Expected: a list including `help`, `cache-info`, `fetch-sdk`, `prep`, `run-tests`, `test`, `clean`.

- [ ] **Step 3: Verify cache-info resolves paths**

Run: `make cache-info`
Expected: `cache`/`sdk`/`toolkit`/`models` lines; `sdk` shows `[absent]` (cache not built yet).

- [ ] **Step 4: Verify host tests run green**

Run: `make run-tests`
Expected: `cache_sh_test` ALL PASS, `fetch_sdk_test` ALL PASS, `test_camera_autodetect` ALL PASS.

- [ ] **Step 5: Commit**

```bash
git add Makefile
git commit -m "build: add Makefile entry point (cache wiring, help, host tests)"
```

---

### Task 4: RKNN prep fetch (`scripts/prep.sh`)

**Files:**
- Create: `scripts/prep.sh`
- Modify: `.gitignore` (add the fetched headers — finalized in Task 9; add the two lines here so the fetch does not dirty the tree)

**Interfaces:**
- Produces: `bash scripts/prep.sh` fetches the RKNN headers the app includes plus `librknnrt.so` into `include/` (idempotent; skips non-empty files). `bash scripts/prep.sh clean` removes exactly those files.

- [ ] **Step 1: Enumerate the exact RKNN headers the app needs**

Run:
```bash
grep -rhoE '#include[ ]*[<"][^">]*rknn[^">]*[>"]' src include | sort -u
grep -rhoE '#include[ ]*[<"][^">]*(rga|drm)[^">]*[>"]' src include | sort -u
```
Record the resulting header names (e.g. `rknn_api.h`, `rknn_matmul_api.h`, and any RGA/DRM headers). These seed the `FILES` map in Step 3. (Today they resolve under `toolkit/rknn-toolkit2/rknpu2/runtime/Linux/librknn_api/include`.)

- [ ] **Step 2: Write `scripts/prep.sh`**

Port from `../argus-all/argus-rtsp-watcher/scripts/prep.sh`, trimming its `FILES` list to the headers found in Step 1 plus `librknnrt.so`. Keep: `RKNN_TAG=v2.3.0`, raw.githubusercontent fetch of each file to `include/`, idempotent skip of non-empty files, and the `clean` subcommand removing exactly those files. Source URLs follow rtsp-watcher's (airockchip `rknn-toolkit2` / `rknn_model_zoo` at `RKNN_TAG`).

- [ ] **Step 3: Verify syntax and a dry fetch**

Run: `bash -n scripts/prep.sh && bash scripts/prep.sh`
Expected: `include/librknnrt.so` and the enumerated headers present; re-running prints skips (idempotent).

- [ ] **Step 4: Verify clean**

Run: `bash scripts/prep.sh clean`
Expected: exactly the fetched files removed; no other `include/` files touched (`git status include/` clean of unexpected deletions).

- [ ] **Step 5: Commit**

```bash
git add scripts/prep.sh .gitignore
git commit -m "build: add prep.sh to fetch RKNN headers + runtime into include/"
```

---

### Task 5: Per-SOC model build (`scripts/build-models.sh`)

**Files:**
- Create: `scripts/build-models.sh`
- Modify: `Makefile` (add `build-models` target)

**Interfaces:**
- Consumes: `scripts/lib/cache.sh` (`TOOLKIT_DIR`, `MODELS_DIR`), the `rknn_tk2` Docker image, `<cache>/toolkit/rknn_model_zoo`.
- Produces: `bash scripts/build-models.sh <models-dir> [SOC...]` writes `<models-dir>/<SOC>/mobilenetv2-embedding-<soc>.rknn`, `<models-dir>/<SOC>/yolox_s_<soc>.rknn`, and `<models-dir>/<SOC>/mobilenetv2_config.json`. Idempotent per SOC via a `models_present()` check.

- [ ] **Step 1: Write the failing test (filename contract + idempotency, mocked)**

```bash
# test/build_models_contract_test.sh
#!/usr/bin/env bash
set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SH="$REPO/scripts/build-models.sh"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail=0

# Pre-populate the expected outputs for RK3588 so the script must SKIP (idempotency)
# without needing Docker. The script must detect a complete SOC and not invoke docker.
mkdir -p "$TMP/RK3588"
: > "$TMP/RK3588/mobilenetv2-embedding-rk3588.rknn"
: > "$TMP/RK3588/yolox_s_rk3588.rknn"
: > "$TMP/RK3588/mobilenetv2_config.json"

# Force any docker call to fail; if the script tries docker on a complete SOC, we catch it.
out="$(PATH="$TMP/nobin:$PATH" DOCKER=/bin/false ARGUS_CACHE_DIR="$TMP/cache" bash "$SH" "$TMP" RK3588 2>&1)"; rc=$?
{ [ "$rc" = 0 ] && echo "$out" | grep -qi "skip\|already"; } \
  && echo "  ok   complete SOC skipped (no docker)" || { echo "  FAIL idempotency rc=$rc out=$out"; fail=1; }

[ "$fail" = 0 ] && echo "ALL PASS" || { echo "FAILURES"; exit 1; }
```

(The script must read a `DOCKER` env override, default `docker`, so this test can prove no container runs on a complete SOC. If rtsp-watcher's script hardcodes `docker`, add the `DOCKER=${DOCKER:-docker}` indirection when porting.)

- [ ] **Step 2: Run the test to verify it fails**

Run: `bash test/build_models_contract_test.sh`
Expected: FAIL (script missing).

- [ ] **Step 3: Write `scripts/build-models.sh`**

Port from `../argus-all/argus-rtsp-watcher/scripts/build-models.sh`. Changes for cerberus:
- Source `scripts/lib/cache.sh`; use `TOOLKIT_DIR` for the `rknn_model_zoo`/`rknn-toolkit2` clones.
- Replace the model map with cerberus's two models, carrying over the numerics from the current `compile-models` (mobilenet: INT8 asymmetric, ImageNet mean/std, 1280-dim embedding; yolox-s: as today). Per SOC run the `rknn_tk2` image to convert, writing:
  - `mobilenetv2-embedding-<soc>.rknn` (from `mobilenetv2-12.onnx`)
  - `yolox_s_<soc>.rknn` (from `yolox_s.onnx`)
  - `mobilenetv2_config.json`
- Keep `models_present()` (all three files exist -> skip the SOC) and build the `rknn_tk2` image only on a miss.
- Add `DOCKER=${DOCKER:-docker}` and use `$DOCKER` for all container calls.
- Write to a temp file and `mv` into place on success so a failed conversion never leaves a truncated `.rknn`.
- Preserve the synthetic calibration-dataset generation the current `compile-models` uses (via `tools/create_synthetic_calibration.py`) if the mobilenet INT8 path needs it.

- [ ] **Step 4: Run the test to verify it passes**

Run: `bash test/build_models_contract_test.sh`
Expected: `ALL PASS`.

- [ ] **Step 5: Add the Makefile target and verify it is wired**

Add to `Makefile`:
```make
build-models:        ## Compile the RKNN models per SoC into the shared cache (needs docker + rknn_tk2 + cache/toolkit)
	bash scripts/build-models.sh $(MODELS_DIR) $(SOC_LIST)
```
Add `build-models` to `.PHONY`. Run: `make -n build-models` and confirm it expands to the script call with the resolved `MODELS_DIR` and `SOC_LIST`.

- [ ] **Step 6: Commit**

```bash
git add scripts/build-models.sh test/build_models_contract_test.sh Makefile
git commit -m "build: add build-models.sh writing per-SoC rknn into the shared cache"
```

> NOTE: a real model compile (`make build-models`) requires the `rknn_tk2` image and `<cache>/toolkit`, produced by `brightsign-sdk-builder`. It is exercised in Task 9's end-to-end verification, not here.

---

### Task 6: Repoint CMakeLists at the cache SDK / fetched RKNN

**Files:**
- Modify: `CMakeLists.txt` (RKNN path ~lines 86-87, 203, 363, 445; dead `copy_models` ~341-359; mosquitto tarball search ~261-263; duplicate install prefix ~78 vs 378; duplicate `install(TARGETS anomaly_detection ...)`)

**Interfaces:**
- Consumes: `OECORE_TARGET_SYSROOT` (from the sourced cache SDK env), `include/librknnrt.so` + RKNN headers (from `prep.sh`).
- Produces: a cross build whose RKNN runtime/headers come from `include/`, installing to `install/${SOC_DIR}` only.

- [ ] **Step 1: Repoint RKNN to `include/`**

Change `RKNN_API_PATH`/`RKNN_RT_LIB` from `${CMAKE_SOURCE_DIR}/toolkit/rknn-toolkit2/rknpu2/runtime/Linux/librknn_api` to use the prep-fetched artifacts, mirroring rtsp-watcher:
```cmake
set(RKNN_RT_LIB ${CMAKE_SOURCE_DIR}/include/librknnrt.so)
include_directories(${CMAKE_SOURCE_DIR}/include)   # rknn_api.h et al. fetched by prep.sh
```
Keep the existing link (`target_link_libraries(... ${RKNN_RT_LIB})`) and the copy of `librknnrt.so` into the build dir and `install/<SOC>/lib`, sourced from `${CMAKE_SOURCE_DIR}/include/librknnrt.so`.

- [ ] **Step 2: Remove the dead `copy_models` CMake target**

Delete the `copy_models` custom target/commands that read `toolkit/mobilenetv2-embedding-${TARGET_SOC}.rknn` at the toolkit root (nothing writes there; staging is handled by `package`). Leave all lib-bundling (GStreamer ext-bundle, OpenCV, TBB, RGA, turbojpeg, mosquitto) untouched.

- [ ] **Step 3: Drop the `brightsign-oe` mosquitto search path**

In the mosquitto tarball lookup (~lines 261-263), remove the `brightsign-oe/build/downloads/` search entry; keep the network download fallback.

- [ ] **Step 4: Collapse duplicate install prefix / targets**

Remove the earlier `CMAKE_INSTALL_PREFIX` assignment (`install/${TARGET_SOC}_linux_aarch64`, ~line 78) and the redundant earlier `install(TARGETS anomaly_detection ... bin)`, keeping only the `install/${SOC_DIR}` prefix and the final root install.

- [ ] **Step 5: Verify the file still parses (configure dry run)**

If the cache SDK is present (`make cache-info` shows `sdk present`):
Run: `source "$(bash scripts/lib/cache.sh print sdk-env)" && mkdir -p build_rk3588 && cmake -S . -B build_rk3588 -DOECORE_TARGET_SYSROOT="$OECORE_TARGET_SYSROOT" -DTARGET_SOC=rk3588 -DCMAKE_BUILD_TYPE=Release`
Expected: configure succeeds; no reference to `toolkit/` or `brightsign-oe/`.
If the cache SDK is absent: run `cmake -P /dev/stdin <<<'message(STATUS "syntax ok")'` is not sufficient — instead defer this verification to Task 9 and note it here.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: source RKNN from include/, drop toolkit/brightsign-oe refs, de-dup install"
```

---

### Task 7: Makefile build/package targets + package model source

**Files:**
- Modify: `Makefile` (add `build-engine`, `package`, `build`)
- Modify: `package` (model source -> `<cache>/models/<SOC>`; build-dir lookups -> `build_<soc>`)

**Interfaces:**
- Consumes: `SDK_ENV` (cache SDK), `MODELS_DIR`, the existing `package` + `sh/make-extension-lvm`.
- Produces: `make build-engine` (per-SOC cross build+install), `make package` (dev zip + LVM ext zip), `make build` (full pipeline).

- [ ] **Step 1: Add `build-engine` to the Makefile**

```make
build-engine: fetch-sdk prep  ## Cross-compile + install for each SoC in SOC_LIST against the cache SDK
	@for soc in $(SOC_LIST); do \
		socl=$$(echo $$soc | tr A-Z a-z); \
		bdir=build_$$socl; \
		echo "== building $$soc in $$bdir =="; \
		mkdir -p $$bdir; \
		bash -c 'source "$(SDK_ENV)" && \
			cmake -S . -B '"$$bdir"' -DOECORE_TARGET_SYSROOT="$$OECORE_TARGET_SYSROOT" -DTARGET_SOC='"$$socl"' -DCMAKE_BUILD_TYPE=Release && \
			$(MAKE) -C '"$$bdir"' -j$$(nproc) && \
			$(MAKE) -C '"$$bdir"' install' || exit 1; \
	done
```

- [ ] **Step 2: Repoint `package`'s model source**

In `package`, change `copy_models` to read `.rknn` from `$(bash scripts/lib/cache.sh print models)/<SOC>/` instead of `install/<SOC>/model`. Source `scripts/lib/cache.sh` near the top of `package` to get `MODELS_DIR`. Update any `build_<platform>` build-dir references to `build_<soc>` so they match Task 1's standardized naming (grep `package` for `build_` and reconcile XT5/LS5/Firebird -> rk3588/rk3568/rk3576).

- [ ] **Step 3: Add `package` and `build` targets**

```make
package: build-engine build-models  ## Stage all SoCs and produce the dev + LVM extension zips
	./package

build: package       ## Full build: fetch SDK, prep, models, cross-compile, package
```
Add `build-engine package build` to `.PHONY`.

- [ ] **Step 4: Verify wiring (dry run, no SDK needed)**

Run: `make -n build-engine package build`
Expected: expands to the cmake/make/package calls with resolved `SDK_ENV`, `SOC_LIST`, `MODELS_DIR`; no `toolkit/` or `build-apps` references.

- [ ] **Step 5: Verify the model-source contract in `package`**

Run: `grep -n "models\|build_" package | sed -n '1,40p'`
Expected: model reads resolve under the cache `models` dir; build-dir references are `build_rk3588/build_rk3568/build_rk3576`; the staged `model/` filenames remain `mobilenetv2-embedding-<soc>.rknn` / `yolox_s_<soc>.rknn`.

- [ ] **Step 6: Commit**

```bash
git add Makefile package
git commit -m "build: Makefile build-engine/package/build targets; package reads models from cache"
```

---

### Task 8: Remove vendored SDK-build files

**Files:**
- Remove (git rm): `build-apps`, `compile-models`, `Dockerfile`, `bsoe-recipes/`, `scripts/runall.sh`, `scripts/clean_build.sh`, `scripts/install-gstreamer-plugins.sh`, `scripts/validate_bbappend.sh`

- [ ] **Step 1: Confirm no surviving references to the removed scripts/files**

Run:
```bash
grep -rnE "build-apps|compile-models|runall\.sh|clean_build\.sh|install-gstreamer-plugins\.sh|validate_bbappend\.sh|bsoe-recipes|[^a-z]Dockerfile" \
  --include='*.sh' --include='Makefile' --include='CMakeLists.txt' --include='package' --include='*.md' . \
  | grep -vE 'docs/superpowers/' | grep -v '\.git/'
```
Expected: no references from live build files (docs/spec/plan mentions are fine). If a live reference remains (e.g. CMake's mosquitto path still hitting `brightsign-oe`), fix it before removal.

- [ ] **Step 2: Confirm the three OE helpers have no non-OE caller**

Run:
```bash
grep -rnE "clean_build\.sh|install-gstreamer-plugins\.sh|validate_bbappend\.sh" --include='*.sh' --include='Makefile' . | grep -v '\.git/'
```
Expected: only self-references / removed `runall.sh`. (Decision already taken: remove.)

- [ ] **Step 3: git rm the files**

```bash
git rm build-apps compile-models Dockerfile
git rm -r bsoe-recipes
git rm scripts/runall.sh scripts/clean_build.sh scripts/install-gstreamer-plugins.sh scripts/validate_bbappend.sh
```

- [ ] **Step 4: Verify host tests + Makefile still sound**

Run: `make run-tests && make -n build`
Expected: host tests pass; `make -n build` still expands cleanly (no reference to removed files).

- [ ] **Step 5: Commit**

```bash
git commit -m "build: remove vendored SDK-build files (now owned by brightsign-sdk-builder)"
```

---

### Task 9: .gitignore, end-to-end verification, blob deletion

**Files:**
- Modify: `.gitignore`

**Interfaces:**
- Consumes: the populated shared cache (SDK 9.1 + toolkit + `rknn_tk2`) from `brightsign-sdk-builder`.

- [ ] **Step 1: Finalize `.gitignore`**

Ensure these are present (keep existing entries): `build_*/`, `install/`, `sdk/`, `toolkit/`, `brightsign-oe/`, and add `/staging/`, `*.rknn`, and the prep-fetched RKNN artifacts under `include/` (the exact header names from Task 4 Step 1, plus `include/librknnrt.so`). Commit:
```bash
git add .gitignore
git commit -m "build: gitignore staging, rknn models, prep-fetched RKNN headers"
```

- [ ] **Step 2: Populate the shared cache (prerequisite, outside this repo)**

Run: `cd ../brightsign-sdk-builder && make build` (multi-hour, ~25 GB first run), then back in cerberus: `make cache-info`.
Expected: `sdk present`, `toolkit present`; `docker image inspect rknn_tk2` succeeds.
(If another consumer already populated the cache, this is a no-op.)

- [ ] **Step 3: Fetch + prep + host tests**

Run: `make fetch-sdk prep run-tests`
Expected: fetch-sdk reports the cache SDK self-consistent; prep populates `include/`; host tests pass.

- [ ] **Step 4: Cross-compile against the cache SDK (clean)**

Run: `make clean && make build-engine`
Expected: RK3588/RK3576/RK3568 build + install succeed. Confirm the new objects are in the binary:
```bash
nm -C build_rk3588/anomaly_detection | grep -E "resolve_camera_device|auto_detect_usb_device_v4l2"
```

- [ ] **Step 5: Build models + package**

Run: `make build-models && make package`
Expected: `<cache>/models/<SOC>/` has `mobilenetv2-embedding-<soc>.rknn` + `yolox_s_<soc>.rknn`; `package` produces `anomaly-detection-dev-*.zip` and `anomaly-detection-ext-*.zip`; staging `model/` dirs contain the correctly named `.rknn`.

- [ ] **Step 6: Verify the extension manifest / staging layout**

Run: `unzip -l anomaly-detection-ext-*.zip | head -40`
Expected: `ext_npu_anomaly.squashfs` + `ext_npu_anomaly_install-lvm.sh` present; per-SOC subdirs carry binary + `lib/` + `model/` + configs.

- [ ] **Step 7: Delete the on-disk blobs (irreversible; only after Steps 2-6 green)**

```bash
rm -rf sdk brightsign-oe toolkit
rm -f brightsign-x86_64-cobra-toolchain-9.0.189.sh
```
Then re-verify the build still works purely from the cache:
Run: `make clean && make build-engine`
Expected: success (proves nothing depended on the deleted in-repo blobs).

- [ ] **Step 8: Final commit (docs/status, if any tracked changes remain)**

```bash
git add -A
git commit -m "build: complete migration to shared SDK cache; drop in-repo SDK blobs"
```

---

## Notes for the executor

- Tasks 1-8 are verifiable **without** the populated cache (host tests, syntax, dry-run wiring, static grep). Task 9 Steps 2-7 require the `brightsign-sdk-builder` 9.1 cache and Docker and must run on a build box.
- Do all work on a feature branch, not `main` (the user commits/merges on their own cadence).
- Ported scripts are the rtsp-watcher originals; preserve their self-consistency checks and error messages, changing only cerberus-specific paths, the model map, and the `DOCKER` indirection.
