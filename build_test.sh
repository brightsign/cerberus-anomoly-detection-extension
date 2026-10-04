#!/bin/bash
#
# Build script for MobileNetV2 test application
# 
# Usage:
#   ./build_test.sh [RK3588|RK3568|RK3576]   (model aliases: XT5, LS5, XS6/XD6/HD6)
#

set -e

# Determine platform
if [ $# -eq 0 ]; then
    PLATFORM="RK3588"
    echo "No platform specified, defaulting to RK3588"
else
    PLATFORM=$1
fi

# Map platform (SOC code or player-model alias) to SOC
case "$PLATFORM" in
    RK3588|XT5)
        SOC="RK3588"
        SDK_TARGET="rk3588"
        ;;
    RK3568|LS5)
        SOC="RK3568"
        SDK_TARGET="rk3568"
        ;;
    RK3576|XS6|XD6|HD6)
        SOC="RK3576"
        SDK_TARGET="rk3576"
        ;;
    *)
        echo "ERROR: Unknown platform: $PLATFORM"
        echo "Usage: $0 [RK3588|RK3568|RK3576]  (aliases: XT5, LS5, XS6/XD6/HD6)"
        exit 1
        ;;
esac

echo "==========================================="
echo "Building MobileNetV2 Test Application"
echo "==========================================="
echo "Platform: $PLATFORM ($SOC)"
echo "==========================================="
echo ""

# Resolve the shared build cache (SDK + toolkit) -- same source of truth as the
# Makefile. The SDK and the RKNN toolkit are provisioned by brightsign-sdk-builder.
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_ENV="$(bash "$REPO_DIR/scripts/lib/cache.sh" print sdk-env)"
TOOLKIT_DIR="$(bash "$REPO_DIR/scripts/lib/cache.sh" print toolkit)"
if [ ! -f "$SDK_ENV" ]; then
    echo "ERROR: cross SDK not found in the shared cache."
    echo "Populate it first:  cd ../brightsign-sdk-builder && make build"
    echo "(or: make fetch-sdk SDK_INSTALLER=/path/to/brightsign-x86_64-cobra-toolchain-*.sh)"
    exit 1
fi

# Ensure the RKNN header + runtime are present in include/ (test includes rknn_api.h).
echo "[1/4] Fetching RKNN sources + setting up SDK environment..."
bash "$REPO_DIR/scripts/prep.sh"
# shellcheck disable=SC1090
source "$SDK_ENV"

# Create build directory
BUILD_DIR="build_test_${SOC,,}"
echo "[2/4] Creating build directory: $BUILD_DIR"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Run CMake
echo "[3/4] Configuring with CMake..."
cmake ../test \
    -DTARGET_PLATFORM="$SOC" \
    -DTOOLKIT_DIR="$TOOLKIT_DIR"

# Build
echo "[4/4] Building..."
make -j$(nproc)

# Install to test directory
INSTALL_DIR="../install_test/$SOC"
mkdir -p "$INSTALL_DIR"
cp test_mobilenet "$INSTALL_DIR/"

cd ..

echo ""
echo "==========================================="
echo "✓ Build completed successfully!"
echo "==========================================="
echo "Executable: install_test/$SOC/test_mobilenet"
echo ""
echo "Next steps:"
echo "  1. Copy to device (model + sample image come from the shared cache):"
echo "     adb push install_test/$SOC/test_mobilenet /userdata/"
echo "     adb push $TOOLKIT_DIR/../models/$SOC/mobilenetv2-embedding-${SOC,,}.rknn /userdata/"
echo "     adb push $TOOLKIT_DIR/rknn_model_zoo/examples/mobilenet/model/bell.jpg /userdata/"
echo ""
echo "  2. Run on device:"
echo "     adb shell"
echo "     cd /userdata"
echo "     chmod +x test_mobilenet"
echo "     export LD_LIBRARY_PATH=/oem/lib:\$LD_LIBRARY_PATH"
echo "     ./test_mobilenet mobilenetv2-embedding-${SOC,,}.rknn bell.jpg"
echo ""
