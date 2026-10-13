#!/usr/bin/env bash
set -euo pipefail

# Host-side unit tests -- no cross SDK, no device, no docker. Run by `make
# run-tests` / `make test`. Covers: cache-path resolution, fetch-sdk guard rails,
# the model-compile contract, the no-stale-sdk-refs guard, the camera autodetect
# C++ unit, the zone-picker JS logic, and the Go config-server.

REPO_ROOT="${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "${REPO_ROOT}"

echo "== shell unit tests =="
bash test/cache_sh_test.sh
bash test/fetch_sdk_test.sh
bash test/build_models_contract_test.sh
bash test/no_stale_sdk_refs_test.sh

echo "== camera autodetect (C++) =="
g++ -std=c++17 -Wall -Iinclude \
    test/test_camera_autodetect.cpp \
    src/wvm/camera_autodetect.cpp \
    src/wvm/logger.cpp \
    -o /tmp/test_camera_autodetect
/tmp/test_camera_autodetect

echo "== zone-picker logic (node) =="
node srv/config-server/web/zone-picker.test.js

echo "== config-server (go) =="
( cd srv/config-server && go test ./... )

echo "ALL HOST TESTS PASSED"
