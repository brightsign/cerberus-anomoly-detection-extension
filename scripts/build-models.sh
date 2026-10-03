#!/usr/bin/env bash
set -euo pipefail

# Compiles the RKNN models (MobileNetV2 embedding + YOLOX-S) per SoC from their
# upstream ONNX sources into the shared cache, so this project does not vendor or
# rebuild them per-repo. Mirrors the retired compile-models: clone
# rknn-toolkit2 + rknn_model_zoo (pinned) if the sdk-builder cache did not, build
# the rknn_tk2 container image, generate the conversion scripts, then convert per
# SoC. The .rknn are SoC-specific (quantized per NPU), one set per SoC under
# <output-dir>/<SOC>/.
#
# Invoked by `make build-models`. Needs docker + network + ~10GB for the image.
#
# Usage: build-models.sh <output-dir> [SOC ...]   (default SOCs: RK3588 RK3576 RK3568)

OUTPUT_DIR="${1:?usage: build-models.sh <output-dir> [SOC ...]}"
shift || true
SOCS=("$@")
if [ "${#SOCS[@]}" -eq 0 ]; then
    SOCS=(RK3588 RK3576 RK3568)
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck disable=SC1091
source "$(dirname "${BASH_SOURCE[0]}")/lib/cache.sh"   # sets CACHE_DIR, TOOLKIT_DIR
ZOO_DIR="${TOOLKIT_DIR}/rknn_model_zoo"
TK2_DIR="${TOOLKIT_DIR}/rknn-toolkit2"
TOOLKIT_REF="${RKNN_TAG:-v2.3.0}"
DOCKER="${DOCKER:-docker}"
IMAGE="rknn_tk2"

MOBILENET_ONNX="mobilenetv2-12.onnx"   # rknn_model_zoo mobilenet example
MOBILENET_RKNN="mobilenetv2-12.rknn"   # name produced inside the container

require_docker() {
    command -v "${DOCKER}" >/dev/null 2>&1 || { echo "ERROR: ${DOCKER} is required but not installed" >&2; exit 1; }
    "${DOCKER}" info >/dev/null 2>&1 || { echo "ERROR: ${DOCKER} daemon is not running" >&2; exit 1; }
}

# A SoC is done when all three staged outputs exist. Checked BEFORE docker so a
# complete cache short-circuits with no container and no toolkit clone.
models_present() {
    local soc_upper="$1" soc_lower
    soc_lower="${soc_upper,,}"
    [ -f "${OUTPUT_DIR}/${soc_upper}/mobilenetv2-embedding-${soc_lower}.rknn" ] &&
        [ -f "${OUTPUT_DIR}/${soc_upper}/yolox_s_${soc_lower}.rknn" ] &&
        [ -f "${OUTPUT_DIR}/${soc_upper}/mobilenetv2_config.json" ]
}

clone_toolkit() {
    mkdir -p "${TOOLKIT_DIR}"
    if [ ! -d "${TK2_DIR}" ]; then
        echo "==> Cloning rknn-toolkit2 (${TOOLKIT_REF})"
        git clone --depth 1 --branch "${TOOLKIT_REF}" https://github.com/airockchip/rknn-toolkit2.git "${TK2_DIR}"
    fi
    if [ ! -d "${ZOO_DIR}" ]; then
        echo "==> Cloning rknn_model_zoo (${TOOLKIT_REF})"
        git clone --depth 1 --branch "${TOOLKIT_REF}" https://github.com/airockchip/rknn_model_zoo.git "${ZOO_DIR}"
    fi
}

build_image() {
    if "${DOCKER}" image inspect "${IMAGE}" >/dev/null 2>&1; then
        echo "==> ${IMAGE} image present"
        return
    fi
    local dockerfile_dir="${TK2_DIR}/rknn-toolkit2/docker/docker_file/ubuntu_20_04_cp38"
    local dockerfile="Dockerfile_ubuntu_20_04_for_cp38"
    if [ ! -f "${dockerfile_dir}/${dockerfile}" ]; then
        echo "ERROR: toolkit Dockerfile not found at ${dockerfile_dir}/${dockerfile}" >&2
        exit 1
    fi
    echo "==> Building ${IMAGE} image (large; one-time)"
    ( cd "${dockerfile_dir}" && "${DOCKER}" build --rm -t "${IMAGE}" -f "${dockerfile}" . )
}

download_mobilenet_onnx() {
    local model_dir="${ZOO_DIR}/examples/mobilenet/model"
    if [ -f "${model_dir}/${MOBILENET_ONNX}" ]; then return; fi
    echo "==> Downloading MobileNetV2 ONNX"
    mkdir -p "${model_dir}"
    if [ -f "${model_dir}/download_model.sh" ]; then
        ( cd "${model_dir}" && bash download_model.sh )
    else
        ( cd "${model_dir}" && wget -q \
            "https://github.com/onnx/models/raw/main/validated/vision/classification/mobilenet/model/${MOBILENET_ONNX}" )
    fi
}

download_yolox_onnx() {
    local model_dir="${ZOO_DIR}/examples/yolox/model"
    if [ -f "${model_dir}/yolox_s.onnx" ]; then return; fi
    echo "==> Downloading YOLOX-S ONNX"
    mkdir -p "${model_dir}"
    ( cd "${model_dir}" && bash download_model.sh )
}

# Synthetic INT8 calibration dataset for the MobileNetV2 quantization, generated
# into the cache toolkit so the container (which mounts TOOLKIT_DIR as /workspace)
# sees /workspace/calibration_dataset.txt and /workspace/calibration_images/*.
# tools/create_synthetic_calibration.py writes paths relative to a literal
# "toolkit/" prefix, so it is run from CACHE_DIR where TOOLKIT_DIR is ./toolkit.
ensure_calibration() {
    if [ -f "${TOOLKIT_DIR}/calibration_dataset.txt" ]; then return; fi
    echo "==> Generating synthetic calibration dataset (testing-grade)"
    ( cd "${CACHE_DIR}" && python3 "${REPO_ROOT}/tools/create_synthetic_calibration.py" )
}

# MobileNetV2: ImageNet mean/std, asymmetric INT8, 1280-dim embedding.
write_mobilenet_convert() {
    local soc_upper="$1"
    local dir="${TOOLKIT_DIR}/models/${soc_upper}"
    mkdir -p "${dir}"
    cat > "${dir}/convert_mobilenetv2.py" << 'PYEOF'
#!/usr/bin/env python3
"""MobileNetV2 ONNX to RKNN: 1280-dim embedding (before classifier)."""
import sys
from rknn.api import RKNN

def convert_model(onnx_path, target_platform, output_path, dataset_path):
    rknn = RKNN(verbose=True)
    rknn.config(
        mean_values=[[123.675, 116.28, 103.53]],
        std_values=[[58.395, 57.12, 57.375]],
        target_platform=target_platform,
        quantized_dtype='asymmetric_quantized-8',
        optimization_level=3,
        quantized_algorithm='normal',
        quantized_method='channel',
    )
    if rknn.load_onnx(model=onnx_path) != 0:
        print('Load ONNX model failed!'); return False
    if rknn.build(do_quantization=True, dataset=dataset_path) != 0:
        print('Build RKNN model failed!'); return False
    if rknn.export_rknn(output_path) != 0:
        print('Export RKNN model failed!'); return False
    rknn.release()
    print('Model successfully converted to: %s' % output_path)
    return True

if __name__ == '__main__':
    if len(sys.argv) != 5:
        print('Usage: convert_mobilenetv2.py <onnx> <platform> <out> <dataset>')
        sys.exit(1)
    sys.exit(0 if convert_model(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]) else 1)
PYEOF
}

# YOLOX-S: raw uint8 RGB, fp32 (no quant) -- startup-only TV rectangle detector.
write_yolox_convert() {
    local soc_upper="$1"
    local dir="${TOOLKIT_DIR}/models/${soc_upper}"
    mkdir -p "${dir}"
    cat > "${dir}/convert_yolox.py" << 'PYEOF'
#!/usr/bin/env python3
"""Convert YOLOX-S ONNX to RKNN (fp32) for NPU-based TV rectangle detection."""
import sys
from rknn.api import RKNN

def main():
    if len(sys.argv) != 4:
        print('Usage: convert_yolox.py <onnx> <platform> <out>'); sys.exit(1)
    onnx_path, platform, output_path = sys.argv[1], sys.argv[2], sys.argv[3]
    rknn = RKNN(verbose=False)
    rknn.config(mean_values=[[0, 0, 0]], std_values=[[1, 1, 1]], target_platform=platform)
    if rknn.load_onnx(model=onnx_path) != 0:
        print('Load ONNX failed!'); sys.exit(1)
    if rknn.build(do_quantization=False) != 0:
        print('Build failed!'); sys.exit(1)
    if rknn.export_rknn(output_path) != 0:
        print('Export failed!'); sys.exit(1)
    rknn.release()
    print('YOLOX-S RKNN model saved: %s' % output_path)

if __name__ == '__main__':
    main()
PYEOF
}

docker_convert() {
    # Run a conversion command inside the rknn_tk2 image with the cache toolkit
    # mounted at /workspace. No -it: there is no TTY under make.
    "${DOCKER}" run --rm -v "${TOOLKIT_DIR}:/workspace" "${IMAGE}" /bin/bash -c "$1"
}

compile_soc() {
    local soc_upper="$1" soc_lower
    soc_lower="${soc_upper,,}"
    local mdir="/workspace/models/${soc_upper}"

    write_mobilenet_convert "${soc_upper}"
    write_yolox_convert "${soc_upper}"

    echo "==> Compiling MobileNetV2 for ${soc_upper}"
    docker_convert "cd /workspace && python models/${soc_upper}/convert_mobilenetv2.py \
        /workspace/rknn_model_zoo/examples/mobilenet/model/${MOBILENET_ONNX} \
        ${soc_lower} ${mdir}/${MOBILENET_RKNN} /workspace/calibration_dataset.txt"
    local mobilenet_out="${TOOLKIT_DIR}/models/${soc_upper}/${MOBILENET_RKNN}"
    [ -f "${mobilenet_out}" ] || { echo "ERROR: MobileNetV2 produced no output for ${soc_upper}" >&2; exit 1; }

    echo "==> Compiling YOLOX-S for ${soc_upper}"
    docker_convert "cd /workspace && python models/${soc_upper}/convert_yolox.py \
        /workspace/rknn_model_zoo/examples/yolox/model/yolox_s.onnx \
        ${soc_lower} ${mdir}/yolox_s_${soc_lower}.rknn"
    local yolox_out="${TOOLKIT_DIR}/models/${soc_upper}/yolox_s_${soc_lower}.rknn"
    [ -f "${yolox_out}" ] || { echo "ERROR: YOLOX-S produced no output for ${soc_upper}" >&2; exit 1; }

    stage_outputs "${soc_upper}" "${mobilenet_out}" "${yolox_out}"
}

# Stage into <output-dir>/<SOC>/ with the names bsext_init/app depend on. Each
# file is written via a temp + mv so a failure never leaves a truncated model.
stage_outputs() {
    local soc_upper="$1" mobilenet_out="$2" yolox_out="$3" soc_lower
    soc_lower="${soc_upper,,}"
    local dst="${OUTPUT_DIR}/${soc_upper}"
    mkdir -p "${dst}"

    install -m 0644 "${mobilenet_out}" "${dst}/.mobilenetv2-embedding-${soc_lower}.rknn.tmp"
    mv "${dst}/.mobilenetv2-embedding-${soc_lower}.rknn.tmp" "${dst}/mobilenetv2-embedding-${soc_lower}.rknn"

    install -m 0644 "${yolox_out}" "${dst}/.yolox_s_${soc_lower}.rknn.tmp"
    mv "${dst}/.yolox_s_${soc_lower}.rknn.tmp" "${dst}/yolox_s_${soc_lower}.rknn"

    cat > "${dst}/mobilenetv2_config.json" << JSON
{
  "model_name": "MobileNetV2",
  "model_type": "embedding_extractor",
  "model_source": "RKNN Model Zoo (Rockchip-validated)",
  "input_size": [224, 224, 3],
  "input_format": "RGB",
  "mean": [123.675, 116.28, 103.53],
  "std": [58.395, 57.12, 57.375],
  "output_dims": 1280,
  "output_layer": "global_average_pooling",
  "quantization": "int8",
  "target_platform": "${soc_lower}",
  "use_case": "Reference matching via cosine similarity on embedding vectors"
}
JSON
    echo "==> models -> ${dst}"
}

main() {
    local pending=() soc
    for soc in "${SOCS[@]}"; do
        if models_present "${soc}"; then
            echo "==> ${soc} models present, skipping"
        else
            pending+=("${soc}")
        fi
    done
    if [ "${#pending[@]}" -eq 0 ]; then
        echo "==> all models already compiled"
        return 0
    fi

    require_docker
    clone_toolkit
    build_image
    download_mobilenet_onnx
    download_yolox_onnx
    ensure_calibration

    for soc in "${pending[@]}"; do
        compile_soc "${soc}"
    done
}

main "$@"
