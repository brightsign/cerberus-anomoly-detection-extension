#!/bin/bash
#
# Build script for MobileNetV2 test application
# 
# Usage:
#   ./build_test.sh [XT5|LS5|Firebird]
#

set -e

# Determine platform
if [ $# -eq 0 ]; then
    PLATFORM="XT5"
    echo "No platform specified, defaulting to XT5"
else
    PLATFORM=$1
fi

# Map platform to SOC
case "$PLATFORM" in
    XT5)
        SOC="RK3588"
        SDK_TARGET="rk3588"
        ;;
    LS5)
        SOC="RK3568"
        SDK_TARGET="rk3568"
        ;;
    Firebird)
        SOC="RK3576"
        SDK_TARGET="rk3576"
        ;;
    *)
        echo "ERROR: Unknown platform: $PLATFORM"
        echo "Usage: $0 [XT5|LS5|Firebird]"
        exit 1
        ;;
esac

echo "==========================================="
echo "Building MobileNetV2 Test Application"
echo "==========================================="
echo "Platform: $PLATFORM ($SOC)"
echo "==========================================="
echo ""

# Check for SDK
SDK_DIR="$(pwd)/sdk"
if [ ! -d "$SDK_DIR" ]; then
    echo "ERROR: SDK not found at $SDK_DIR"
    echo "Please install the BrightSign SDK first:"
    echo "  ./brightsign-x86_64-*-toolchain-*.sh -d ./sdk -y"
    exit 1
fi

# Setup SDK environment
echo "[1/4] Setting up SDK environment..."
source "$SDK_DIR/environment-setup-aarch64-oe-linux"

# Create build directory
BUILD_DIR="build_test_${SOC,,}"
echo "[2/4] Creating build directory: $BUILD_DIR"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Run CMake
echo "[3/4] Configuring with CMake..."
cmake ../test \
    -DTARGET_PLATFORM="$SOC"

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
echo "  1. Copy to device:"
echo "     adb push install_test/$SOC/test_mobilenet /userdata/"
echo "     adb push install/$SOC/model/mobilenetv2-12.rknn /userdata/"
echo "     adb push toolkit/rknn_model_zoo/examples/mobilenet/model/bell.jpg /userdata/"
echo ""
echo "  2. Run on device:"
echo "     adb shell"
echo "     cd /userdata"
echo "     chmod +x test_mobilenet"
echo "     export LD_LIBRARY_PATH=/oem/lib:\$LD_LIBRARY_PATH"
echo "     ./test_mobilenet mobilenetv2-12.rknn bell.jpg"
echo ""
