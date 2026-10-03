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
CACHE_SH := ARGUS_CACHE_DIR='$(ARGUS_CACHE_DIR)' bash scripts/lib/cache.sh print
CACHE_DIR   := $(shell $(CACHE_SH) cache)
SDK_DIR     := $(shell $(CACHE_SH) sdk)
SDK_ENV     := $(shell $(CACHE_SH) sdk-env)
TOOLKIT_DIR := $(shell $(CACHE_SH) toolkit)
MODELS_DIR  := $(shell $(CACHE_SH) models)
export ARGUS_CACHE_DIR

# Per-SoC install dirs (RK3588=XT5, RK3576=Firebird, RK3568=LS5). Build dirs are
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

run-tests:           ## Run host unit tests (no cross SDK needed)
	bash test/cache_sh_test.sh
	bash test/fetch_sdk_test.sh
	bash test/build_models_contract_test.sh
	g++ -std=c++17 -Wall -Iinclude test/test_camera_autodetect.cpp src/wvm/camera_autodetect.cpp src/wvm/logger.cpp -o /tmp/test_camera_autodetect
	/tmp/test_camera_autodetect

test: run-tests      ## Alias for run-tests

clean:               ## Remove build artifacts (build_*/ install/ staging/ zips + prep headers)
	rm -rf build_* install staging *.zip
	bash scripts/prep.sh clean 2>/dev/null || true

.PHONY: help cache-info fetch-sdk prep build-models run-tests test clean
