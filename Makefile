# Cerberus anomaly-detection extension -- build entry point.
#
# The cross SDK, the RKNN toolkit, and the rknn_tk2 model-compile image are
# provisioned once per build box by the sibling brightsign-sdk-builder repo into
# a shared cache OUTSIDE this repo (default ../argus-build-cache, override with
# ARGUS_CACHE_DIR). scripts/lib/cache.sh is the single source of truth for those
# paths; the shell-outs below ask it for the resolved absolute paths so Make and
# the scripts agree.
#
# ARGUS_CACHE_DIR is forwarded explicitly to $(shell ...) because a command-line
# override is a Make variable, not an env var, and exported so the scripts (which
# source cache.sh themselves) resolve the same location.
# This repo sits beside the argus projects' argus-all/ dir rather than inside it,
# so the shared cache (argus-all/argus-build-cache) is not at the default
# ../argus-build-cache. Auto-discover that sibling cache when it holds a built SDK
# and ARGUS_CACHE_DIR was not set explicitly. An explicit override always wins.
ifeq ($(strip $(ARGUS_CACHE_DIR)),)
  _argus_all_cache := $(abspath $(CURDIR)/../argus-all/argus-build-cache)
  ifneq ($(wildcard $(_argus_all_cache)/sdk/environment-setup-aarch64-oe-linux),)
    ARGUS_CACHE_DIR := $(_argus_all_cache)
  endif
endif

CACHE_SH := ARGUS_CACHE_DIR='$(ARGUS_CACHE_DIR)' bash scripts/lib/cache.sh print
CACHE_DIR   := $(shell $(CACHE_SH) cache)
SDK_DIR     := $(shell $(CACHE_SH) sdk)
SDK_ENV     := $(shell $(CACHE_SH) sdk-env)
TOOLKIT_DIR := $(shell $(CACHE_SH) toolkit)
MODELS_DIR  := $(shell $(CACHE_SH) models)
export ARGUS_CACHE_DIR

# Per-SoC install dirs (RK3588=XT5, RK3576=XS6/XD6, RK3568=LS5). Build dirs are
# build_<soc> (lowercase).
SOC_LIST ?= RK3588 RK3576 RK3568

# Optional explicit path to a brightsign-x86_64-cobra-toolchain-*.sh installer.
# If empty, fetch-sdk searches the cache dir / repo root / CWD, and otherwise
# directs you to brightsign-sdk-builder.
SDK_INSTALLER ?=

.DEFAULT_GOAL := help

help:                ## Print available targets
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | awk 'BEGIN{FS=":.*?## "}{printf "  %-14s %s\n", $$1, $$2}'

cache-info:          ## Show the resolved shared-cache location and what is present
	@echo "ARGUS_CACHE_DIR : $(if $(ARGUS_CACHE_DIR),$(ARGUS_CACHE_DIR) (override),(unset -> default))"
	@echo "cache   : $(CACHE_DIR)"
	@echo "sdk     : $(SDK_DIR)  [$(if $(wildcard $(SDK_ENV)),present,absent)]"
	@echo "toolkit : $(TOOLKIT_DIR)  [$(if $(wildcard $(TOOLKIT_DIR)),present,absent)]"
	@echo "models  : $(MODELS_DIR)  [$(if $(wildcard $(MODELS_DIR)),present,absent)]"

fetch-sdk:           ## Ensure the aarch64 cross SDK is present in the shared cache
	bash scripts/fetch-sdk.sh $(SDK_INSTALLER)

prep:                ## Fetch RKNN headers + runtime into include/ (needs network)
	bash scripts/prep.sh

build-models:        ## Compile the RKNN models per SoC into the shared cache (needs docker + rknn_tk2 + cache/toolkit)
	bash scripts/build-models.sh $(MODELS_DIR) $(SOC_LIST)

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

package: build-engine build-models  ## Stage all SoCs and produce the dev + LVM extension zips
	./package

build: package       ## Full build: fetch SDK, prep, compile models, cross-compile, package

copy:                ## scp the most recent extension zip to the player (BS_PLAYER/BS_PASSWORD, e.g. in .envrc)
	@set -e; \
	[ -f .envrc ] && . ./.envrc || true; \
	: "$${BS_PLAYER:?set BS_PLAYER (player ip/hostname), e.g. in .envrc}"; \
	zip=$$(ls -t anomaly-detection-ext-*.zip 2>/dev/null | head -1); \
	[ -n "$$zip" ] || { echo "No anomaly-detection-ext-*.zip found -- run 'make package' first." >&2; exit 1; }; \
	echo "Copying $$zip -> brightsign@$${BS_PLAYER}:/storage/sd/"; \
	: "NOTE: no 'scp -O'. The player's SSH login is the BrightSign REPL, not a"; \
	: "Unix shell; legacy SCP (-O) runs 'scp -t' through that REPL and fails with"; \
	: "'Unknown command: -c scp -t'. Default scp uses the SFTP subsystem, which"; \
	: "dropbear serves independently of the REPL, so it works."; \
	scp_opts="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"; \
	if command -v sshpass >/dev/null 2>&1 && [ -n "$${BS_PASSWORD:-}" ]; then \
		sshpass -p "$${BS_PASSWORD}" scp $$scp_opts "$$zip" "brightsign@$${BS_PLAYER}:/storage/sd/"; \
	else \
		echo "(sshpass unavailable or BS_PASSWORD unset -- you'll be prompted for the password)"; \
		scp $$scp_opts "$$zip" "brightsign@$${BS_PLAYER}:/storage/sd/"; \
	fi; \
	echo ""; \
	echo "Copied. To install, SSH to the player, descend to the root shell, then:"; \
	echo "  cd /usr/local && unzip -o /storage/sd/$$zip && bash ./ext_npu_anomaly_install-lvm.sh && exit"

run-tests:           ## Run host unit tests (no cross SDK needed)
	bash test/cache_sh_test.sh
	bash test/fetch_sdk_test.sh
	bash test/build_models_contract_test.sh
	bash test/no_stale_sdk_refs_test.sh
	g++ -std=c++17 -Wall -Iinclude test/test_camera_autodetect.cpp src/wvm/camera_autodetect.cpp src/wvm/logger.cpp -o /tmp/test_camera_autodetect
	/tmp/test_camera_autodetect
	node srv/config-server/web/zone-picker.test.js
	cd srv/config-server && go test ./...

test: run-tests      ## Alias for run-tests

clean:               ## Remove build artifacts (build_*/ install/ staging/ zips + prep headers)
	rm -rf build_rk* build_test_* install staging *.zip
	bash scripts/prep.sh clean 2>/dev/null || true

.PHONY: help cache-info fetch-sdk prep build-models build-engine package build copy run-tests test clean
