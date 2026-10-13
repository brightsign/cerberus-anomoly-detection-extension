# Cerberus anomaly-detection extension -- build entry point.
#
# The cross SDK, the RKNN toolkit, and the rknn_tk2 model-compile image are
# provisioned once per build box by the sibling brightsign-sdk-builder repo into
# a shared cache OUTSIDE this repo. scripts/lib/cache.sh is the single source of
# truth for those paths; the shell-outs below ask it for resolved absolute paths
# so Make and the scripts agree.
#
# Point the build at the cache with BS_CACHE_DIR, normally set in a git-ignored
# cache.env (see cache.env.example and docs/PRE-REQS.md). BS_CACHE_DIR is
# forwarded explicitly to $(shell ...) because a command-line/-include override
# is a Make variable, not an env var, and exported so the scripts (which source
# cache.sh themselves) resolve the same location.

# Box-specific cache location. Missing file is not an error (-include); a
# BS_CACHE_DIR= on the command line still overrides it.
-include cache.env

# This repo sits beside the argus projects' argus-all/ dir rather than inside it,
# so the shared cache (argus-all/argus-build-cache) is not at cache.sh's default
# ../argus-build-cache. When BS_CACHE_DIR was not set (no cache.env, no CLI
# override), auto-discover that sibling cache if it holds a built SDK. An
# explicit BS_CACHE_DIR always wins.
ifeq ($(strip $(BS_CACHE_DIR)),)
  _argus_all_cache := $(abspath $(CURDIR)/../argus-all/argus-build-cache)
  ifneq ($(wildcard $(_argus_all_cache)/sdk/environment-setup-aarch64-oe-linux),)
    BS_CACHE_DIR := $(_argus_all_cache)
  endif
endif

CACHE_SH := BS_CACHE_DIR='$(BS_CACHE_DIR)' bash scripts/lib/cache.sh print
CACHE_DIR   := $(shell $(CACHE_SH) cache)
SDK_DIR     := $(shell $(CACHE_SH) sdk)
SDK_ENV     := $(shell $(CACHE_SH) sdk-env)
TOOLKIT_DIR := $(shell $(CACHE_SH) toolkit)
MODELS_DIR  := $(shell $(CACHE_SH) models)
# Export so the scripts and ./package resolve the same cache + SDK.
export BS_CACHE_DIR SDK_DIR SDK_ENV
# Auto-build controls (read by scripts/fetch-sdk.sh): if the configured cache has
# no SDK, fetch-sdk builds it via brightsign-sdk-builder. Set BS_SDK_AUTOBUILD=0
# to disable (get manual instructions instead); point SDK_BUILDER_DIR at a local
# brightsign-sdk-builder checkout to skip auto-discovery/clone. Both may be set in
# cache.env.
export BS_SDK_AUTOBUILD SDK_BUILDER_DIR

# Per-SoC install dirs (RK3588=XT5, RK3576=XS6/XD6, RK3568=LS5). Build dirs are
# build_<soc> (lowercase).
SOC_LIST ?= RK3588 RK3576 RK3568

# Optional explicit path to a brightsign-x86_64-cobra-toolchain-*.sh installer.
# If empty, fetch-sdk searches the cache dir / repo root / CWD, and otherwise
# directs you to brightsign-sdk-builder.
SDK_INSTALLER ?=

.DEFAULT_GOAL := help

help:                ## Print available targets
	@grep -hE '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | awk 'BEGIN{FS=":.*?## "}{printf "  %-14s %s\n", $$1, $$2}'
	@echo ""
	@echo "Run 'make cache-info' to see the resolved shared-cache location."
	@echo "First time on this box? Read docs/PRE-REQS.md."

cache-info:          ## Show the resolved shared-cache location and what is present
	@echo "BS_CACHE_DIR : $(if $(BS_CACHE_DIR),$(BS_CACHE_DIR) (set),(unset -> cache.sh default))"
	@echo "cache   : $(CACHE_DIR)"
	@echo "sdk     : $(SDK_DIR)  [$(if $(wildcard $(SDK_ENV)),present,absent)]"
	@echo "toolkit : $(TOOLKIT_DIR)  [$(if $(wildcard $(TOOLKIT_DIR)),present,absent)]"
	@echo "models  : $(MODELS_DIR)  [$(if $(wildcard $(MODELS_DIR)),present,absent)]"

fetch-sdk:           ## Ensure the aarch64 cross SDK is present in the shared cache
	bash scripts/fetch-sdk.sh $(SDK_INSTALLER)

sdk-libs: fetch-sdk  ## Verify the SDK sysroot ships the libraries the engine links (OpenCV, GStreamer, ...)
	bash scripts/build-sdk-libs.sh

prep:                ## Fetch RKNN headers + runtime into include/ (needs network)
	bash scripts/prep.sh

build-models:        ## Compile the RKNN models per SoC into the shared cache (needs docker + rknn_tk2 + cache/toolkit)
	bash scripts/build-models.sh $(MODELS_DIR) $(SOC_LIST)

build-engine: fetch-sdk sdk-libs prep  ## Cross-compile + install for each SoC in SOC_LIST against the cache SDK
	bash scripts/build.sh $(SOC_LIST)

package: build-engine build-models  ## Stage all SoCs and produce the dev + LVM extension zips
	./package

build: package       ## Full build: fetch SDK, verify libs, prep, compile models, cross-compile, package

copy:                ## scp the newest ext zip + install-on-player.sh to the player (BS_PLAYER/BS_PASSWORD in .envrc)
	@set -e; \
	[ -f .envrc ] && . ./.envrc || true; \
	: "$${BS_PLAYER:?set BS_PLAYER (player ip/hostname) in .envrc}"; \
	: "$${BS_PASSWORD:?set BS_PASSWORD (ssh password for user brightsign) in .envrc}"; \
	command -v sshpass >/dev/null 2>&1 || { echo "ERROR: sshpass not installed -- 'sudo apt install sshpass'" >&2; exit 1; }; \
	zip=$$(ls -t anomaly-detection-ext-*.zip 2>/dev/null | head -1); \
	[ -n "$$zip" ] || { echo "ERROR: no anomaly-detection-ext-*.zip found -- run 'make package' first." >&2; exit 1; }; \
	[ -f install-on-player.sh ] || { echo "ERROR: install-on-player.sh missing -- run 'make package' first." >&2; exit 1; }; \
	echo "Copying $$zip + install-on-player.sh -> brightsign@$${BS_PLAYER}:/storage/sd/"; \
	: "No 'scp -O': the player's SSH login is the BrightSign REPL, not a Unix shell;"; \
	: "legacy SCP (-O) runs 'scp -t' through the REPL -> 'Unknown command: -c scp -t'."; \
	: "Default scp uses the SFTP subsystem, which dropbear serves independently."; \
	: "No 'scp -p' either: the player's SFTP server rejects setting remote file"; \
	: "attributes ('remote fsetstat: Permission denied' on /storage/sd)."; \
	sshpass -p "$${BS_PASSWORD}" scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
		"$$zip" install-on-player.sh "brightsign@$${BS_PLAYER}:/storage/sd/" \
		&& { echo ""; \
		     echo "Copied. Install from the player's ROOT shell (Ctrl-C,Enter -> exit -> exit):"; \
		     echo "  sh /storage/sd/install-on-player.sh"; } \
		|| { echo "ERROR: scp to the player failed" >&2; exit 1; }

run-tests:           ## Run host unit tests (no cross SDK needed)
	bash scripts/setup_and_run_tests.sh

test: run-tests      ## Alias for run-tests

clean:               ## Remove build artifacts (build_*/ install/ staging/ downloads/ zips + prep headers)
	rm -rf build_rk* build_test_* install staging downloads *.zip install-on-player.sh
	bash scripts/prep.sh clean 2>/dev/null || true

.PHONY: help cache-info fetch-sdk sdk-libs prep build-models build-engine package build copy run-tests test clean
