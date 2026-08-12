#!/bin/bash
set -euo pipefail

CASE_NAME=$1
BUILD_SCRIPT=$2
TEST_ROOT=$(mktemp -d /tmp/voxorchestra-board-build-test.XXXXXX)
trap 'rm -rf "$TEST_ROOT"' EXIT

FAKE_BIN="$TEST_ROOT/bin"
COMMAND_LOG="$TEST_ROOT/commands.log"
mkdir -p "$FAKE_BIN"
export COMMAND_LOG

printf '%s\n' \
  '#!/bin/bash' \
  'name=$(basename "$0")' \
  'printf "%s" "$name" >> "$COMMAND_LOG"' \
  'printf " %q" "$@" >> "$COMMAND_LOG"' \
  'printf "\n" >> "$COMMAND_LOG"' \
  'if [ "$name" = nproc ]; then echo 64; fi' \
  'exit 0' \
  > "$FAKE_BIN/command_stub"
chmod +x "$FAKE_BIN/command_stub"
for command_name in bash cmake ctest g++ nproc pkg-config; do
  ln -s command_stub "$FAKE_BIN/$command_name"
done
export PATH="$FAKE_BIN:/usr/bin:/bin"

case "$CASE_NAME" in
  default)
    /bin/bash "$BUILD_SCRIPT" default
    grep -Fq -- 'cmake -S . -B build-taishanpi3m -DCMAKE_BUILD_TYPE=Release -DVOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=OFF' "$COMMAND_LOG"
    grep -Fq -- 'cmake --build build-taishanpi3m -j4' "$COMMAND_LOG"
    grep -Fq -- 'ctest --test-dir build-taishanpi3m --output-on-failure' "$COMMAND_LOG"
    grep -Fq -- 'bash scripts/check_no_hw_deps.sh build-taishanpi3m' "$COMMAND_LOG"
    ;;
  hardware)
    SHERPA_ROOT="$TEST_ROOT/sherpa"
    RKLLM_ROOT="$TEST_ROOT/rkllm"
    TTS_ROOT="$TEST_ROOT/summertts"
    ASR_MODEL="$TEST_ROOT/models/asr"
    RKLLM_MODEL="$TEST_ROOT/models/model.rkllm"
    TTS_MODEL="$TEST_ROOT/models/tts.bin"
    mkdir -p \
      "$SHERPA_ROOT/sherpa-onnx/c-api" "$SHERPA_ROOT/build/lib" \
      "$RKLLM_ROOT/include" "$RKLLM_ROOT/aarch64" \
      "$TTS_ROOT/src" "$TTS_ROOT/include" "$TTS_ROOT/eigen-3.4.0" \
      "$ASR_MODEL"
    touch \
      "$SHERPA_ROOT/sherpa-onnx/c-api/c-api.h" \
      "$SHERPA_ROOT/build/lib/libsherpa-onnx-c-api.so" \
      "$RKLLM_ROOT/include/rkllm.h" \
      "$RKLLM_ROOT/aarch64/librkllmrt.so" \
      "$RKLLM_MODEL" "$TTS_MODEL"

    VOXORCHESTRA_SHERTA_ROOT="$SHERPA_ROOT" \
    VOXORCHESTRA_RKLLM_ROOT="$RKLLM_ROOT" \
    VOXORCHESTRA_SUMMERTTS_ROOT="$TTS_ROOT" \
    VOXORCHESTRA_ASR_MODEL="$ASR_MODEL" \
    VOXORCHESTRA_RKLLM_MODEL="$RKLLM_MODEL" \
    VOXORCHESTRA_TTS_MODEL="$TTS_MODEL" \
      /bin/bash "$BUILD_SCRIPT" hardware

    grep -Fq -- 'cmake -S . -B build-taishanpi3m-hw -DCMAKE_BUILD_TYPE=Release -DVOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=ON' "$COMMAND_LOG"
    grep -Fq -- "-DVOXORCHESTRA_SHERTA_ROOT=$SHERPA_ROOT" "$COMMAND_LOG"
    grep -Fq -- "-DVOXORCHESTRA_RKLLM_ROOT=$RKLLM_ROOT" "$COMMAND_LOG"
    grep -Fq -- "-DVOXORCHESTRA_SUMMERTTS_ROOT=$TTS_ROOT" "$COMMAND_LOG"
    grep -Fq -- "-DVOXORCHESTRA_ASR_MODEL=$ASR_MODEL" "$COMMAND_LOG"
    grep -Fq -- "-DVOXORCHESTRA_RKLLM_MODEL=$RKLLM_MODEL" "$COMMAND_LOG"
    grep -Fq -- "-DVOXORCHESTRA_TTS_MODEL=$TTS_MODEL" "$COMMAND_LOG"
    grep -Fq -- 'cmake --build build-taishanpi3m-hw -j4' "$COMMAND_LOG"
    grep -Fq -- 'ctest --test-dir build-taishanpi3m-hw --output-on-failure' "$COMMAND_LOG"
    if grep -Fq 'check_no_hw_deps.sh' "$COMMAND_LOG"; then
      echo '硬件构建不应执行无硬件依赖检查' >&2
      exit 1
    fi
    ;;
  missing)
    set +e
    OUTPUT=$(env \
      -u VOXORCHESTRA_SHERTA_ROOT \
      -u VOXORCHESTRA_RKLLM_ROOT \
      -u VOXORCHESTRA_SUMMERTTS_ROOT \
      -u VOXORCHESTRA_ASR_MODEL \
      -u VOXORCHESTRA_RKLLM_MODEL \
      -u VOXORCHESTRA_TTS_MODEL \
      /bin/bash "$BUILD_SCRIPT" hardware 2>&1)
    STATUS=$?
    set -e
    printf '%s\n' "$OUTPUT"
    if [ "$STATUS" -eq 0 ]; then
      echo '缺失硬件依赖时构建脚本错误返回成功' >&2
      exit 1
    fi
    grep -Fq '缺少硬件构建参数 VOXORCHESTRA_SHERTA_ROOT' <<< "$OUTPUT"
    if grep -q '^cmake ' "$COMMAND_LOG"; then
      echo '硬件依赖预检失败后不应执行 CMake' >&2
      exit 1
    fi
    ;;
  *)
    echo "未知测试场景: $CASE_NAME" >&2
    exit 2
    ;;
esac
