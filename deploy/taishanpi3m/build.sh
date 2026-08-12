#!/usr/bin/env bash
# 泰山派 3M 板端原生构建（aarch64）+ 全量 CTest。
#
# 用法：bash deploy/taishanpi3m/build.sh [default|hardware]
# 产物：default -> build-taishanpi3m/；hardware -> build-taishanpi3m-hw/。
set -euo pipefail
cd "$(dirname "$0")/../.."

MODE=${1:-default}
case "$MODE" in
  default)
    BUILD_DIR=build-taishanpi3m
    HARDWARE_BACKENDS=OFF
    ;;
  hardware)
    BUILD_DIR=build-taishanpi3m-hw
    HARDWARE_BACKENDS=ON
    ;;
  *)
    echo "用法: bash deploy/taishanpi3m/build.sh [default|hardware]" >&2
    exit 2
    ;;
esac

JOBS=${VOXORCHESTRA_BUILD_JOBS:-$(nproc)}
if ! [[ "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo "VOXORCHESTRA_BUILD_JOBS 必须是正整数" >&2
  exit 2
fi
if [ "$JOBS" -gt 4 ]; then
  JOBS=4
fi

require_hardware_parameter() {
  local name=$1
  if [ -z "${!name:-}" ]; then
    echo "缺少硬件构建参数 $name" >&2
    exit 1
  fi
}

require_path() {
  local path=$1
  local description=$2
  if [ ! -e "$path" ]; then
    echo "缺少$description: $path" >&2
    exit 1
  fi
}

if [ "$MODE" = hardware ]; then
  for name in \
    VOXORCHESTRA_SHERTA_ROOT \
    VOXORCHESTRA_RKLLM_ROOT \
    VOXORCHESTRA_SUMMERTTS_ROOT \
    VOXORCHESTRA_ASR_MODEL \
    VOXORCHESTRA_RKLLM_MODEL \
    VOXORCHESTRA_TTS_MODEL; do
    require_hardware_parameter "$name"
  done

  require_path "$VOXORCHESTRA_SHERTA_ROOT/sherpa-onnx/c-api/c-api.h" " sherpa-onnx C API 头文件"
  require_path "$VOXORCHESTRA_SHERTA_ROOT/build/lib/libsherpa-onnx-c-api.so" " sherpa-onnx 动态库"
  require_path "$VOXORCHESTRA_RKLLM_ROOT/include/rkllm.h" " RKLLM 头文件"
  require_path "$VOXORCHESTRA_RKLLM_ROOT/aarch64/librkllmrt.so" " RKLLM Runtime"
  require_path "$VOXORCHESTRA_SUMMERTTS_ROOT/src" " SummerTTS 源码目录"
  require_path "$VOXORCHESTRA_SUMMERTTS_ROOT/include" " SummerTTS 头文件目录"
  require_path "$VOXORCHESTRA_SUMMERTTS_ROOT/eigen-3.4.0" " Eigen 目录"
  require_path "$VOXORCHESTRA_ASR_MODEL" " ASR 模型目录"
  require_path "$VOXORCHESTRA_RKLLM_MODEL" " RKLLM 模型"
  require_path "$VOXORCHESTRA_TTS_MODEL" " TTS 模型"
fi

echo "== 依赖检查 =="
for cmd in cmake g++; do
  if ! command -v "$cmd" >/dev/null; then
    echo "缺少 $cmd：请先安装（sudo apt install -y cmake g++ libzmq3-dev nlohmann-json3-dev）"
    exit 1
  fi
done
if ! pkg-config --exists libzmq 2>/dev/null && [ ! -e /usr/include/zmq.hpp ]; then
  echo "缺少 libzmq3-dev：请先安装（sudo apt install -y libzmq3-dev）"
  exit 1
fi
echo "cmake/g++/libzmq 就绪"

CMAKE_ARGS=(
  -S .
  -B "$BUILD_DIR"
  -DCMAKE_BUILD_TYPE=Release
  "-DVOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=$HARDWARE_BACKENDS"
)
if [ "$MODE" = hardware ]; then
  CMAKE_ARGS+=(
    "-DVOXORCHESTRA_SHERTA_ROOT=$VOXORCHESTRA_SHERTA_ROOT"
    "-DVOXORCHESTRA_RKLLM_ROOT=$VOXORCHESTRA_RKLLM_ROOT"
    "-DVOXORCHESTRA_SUMMERTTS_ROOT=$VOXORCHESTRA_SUMMERTTS_ROOT"
    "-DVOXORCHESTRA_ASR_MODEL=$VOXORCHESTRA_ASR_MODEL"
    "-DVOXORCHESTRA_RKLLM_MODEL=$VOXORCHESTRA_RKLLM_MODEL"
    "-DVOXORCHESTRA_TTS_MODEL=$VOXORCHESTRA_TTS_MODEL"
  )
  if [ -n "${VOXORCHESTRA_ALSA_DEVICE:-}" ]; then
    CMAKE_ARGS+=("-DVOXORCHESTRA_ALSA_DEVICE=$VOXORCHESTRA_ALSA_DEVICE")
  fi
fi

echo "== 配置（Release / $MODE）=="
cmake "${CMAKE_ARGS[@]}"

echo "== 构建（-j$JOBS）=="
cmake --build "$BUILD_DIR" -j"$JOBS"

echo "== 全量测试 =="
ctest --test-dir "$BUILD_DIR" --output-on-failure

if [ "$MODE" = default ]; then
  echo "== 无硬件依赖验收 =="
  bash scripts/check_no_hw_deps.sh "$BUILD_DIR"
fi
