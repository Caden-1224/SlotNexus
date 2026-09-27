#!/bin/bash
# Author: Caden
set -euo pipefail

CASE_NAME=$1
BUILD_SCRIPT=$2
TEST_ROOT=$(mktemp -d /tmp/slotnexus-board-build-test.XXXXXX)
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
    grep -Fq -- 'cmake -S . -B build-taishanpi3m -DCMAKE_BUILD_TYPE=Release -DSLOTNEXUS_ENABLE_HARDWARE_BACKENDS=OFF' "$COMMAND_LOG"
    grep -Fq -- 'cmake --build build-taishanpi3m -j4' "$COMMAND_LOG"
    grep -Fq -- 'ctest --test-dir build-taishanpi3m --output-on-failure' "$COMMAND_LOG"
    grep -Fq -- 'bash scripts/check_no_hw_deps.sh build-taishanpi3m' "$COMMAND_LOG"
    ;;
  hardware)
    SHERPA_ROOT="$TEST_ROOT/sherpa"
    RKLLM_ROOT="$TEST_ROOT/rkllm"
    MELOTTS_ROOT="$TEST_ROOT/melotts"
    ASR_MODEL="$TEST_ROOT/models/asr"
    VOICE_FIXTURE_DIR="$TEST_ROOT/fixtures"
    RKLLM_MODEL="$TEST_ROOT/models/model.rkllm"
    MELOTTS_ENCODER="$TEST_ROOT/models/encoder-zh.onnx"
    MELOTTS_DECODER="$TEST_ROOT/models/decoder-zh.rknn"
    MELOTTS_LEXICON="$TEST_ROOT/models/lexicon.txt"
    MELOTTS_TOKENS="$TEST_ROOT/models/tokens.txt"
    MELOTTS_G="$TEST_ROOT/models/g-zh_mix_en.bin"
    mkdir -p \
      "$SHERPA_ROOT/sherpa-onnx/c-api" "$SHERPA_ROOT/build/lib" \
      "$RKLLM_ROOT/include" "$RKLLM_ROOT/aarch64" \
      "$MELOTTS_ROOT/include" "$MELOTTS_ROOT/lib" \
      "$ASR_MODEL" "$VOICE_FIXTURE_DIR"
    touch \
      "$SHERPA_ROOT/sherpa-onnx/c-api/c-api.h" \
      "$SHERPA_ROOT/build/lib/libsherpa-onnx-c-api.so" \
      "$RKLLM_ROOT/include/rkllm.h" \
      "$RKLLM_ROOT/aarch64/librkllmrt.so" \
      "$MELOTTS_ROOT/include/onnxruntime_c_api.h" \
      "$MELOTTS_ROOT/include/rknn_api.h" \
      "$MELOTTS_ROOT/lib/libonnxruntime.so" \
      "$MELOTTS_ROOT/lib/librknnrt.so" \
      "$RKLLM_MODEL" "$MELOTTS_ENCODER" "$MELOTTS_DECODER" \
      "$MELOTTS_LEXICON" "$MELOTTS_TOKENS" "$MELOTTS_G" \
      "$VOICE_FIXTURE_DIR/demo_zh.wav"

    SLOTNEXUS_SHERTA_ROOT="$SHERPA_ROOT" \
    SLOTNEXUS_RKLLM_ROOT="$RKLLM_ROOT" \
    SLOTNEXUS_MELOTTS_ROOT="$MELOTTS_ROOT" \
    SLOTNEXUS_ASR_MODEL="$ASR_MODEL" \
    SLOTNEXUS_VOICE_FIXTURE_DIR="$VOICE_FIXTURE_DIR" \
    SLOTNEXUS_RKLLM_MODEL="$RKLLM_MODEL" \
    SLOTNEXUS_MELOTTS_ENCODER="$MELOTTS_ENCODER" \
    SLOTNEXUS_MELOTTS_DECODER="$MELOTTS_DECODER" \
    SLOTNEXUS_MELOTTS_LEXICON="$MELOTTS_LEXICON" \
    SLOTNEXUS_MELOTTS_TOKENS="$MELOTTS_TOKENS" \
    SLOTNEXUS_MELOTTS_G="$MELOTTS_G" \
      /bin/bash "$BUILD_SCRIPT" hardware

    grep -Fq -- 'cmake -S . -B build-taishanpi3m-hw -DCMAKE_BUILD_TYPE=Release -DSLOTNEXUS_ENABLE_HARDWARE_BACKENDS=ON' "$COMMAND_LOG"
    grep -Fq -- "-DSLOTNEXUS_SHERTA_ROOT=$SHERPA_ROOT" "$COMMAND_LOG"
    grep -Fq -- "-DSLOTNEXUS_RKLLM_ROOT=$RKLLM_ROOT" "$COMMAND_LOG"
    grep -Fq -- "-DSLOTNEXUS_MELOTTS_ROOT=$MELOTTS_ROOT" "$COMMAND_LOG"
    grep -Fq -- "-DSLOTNEXUS_ASR_MODEL=$ASR_MODEL" "$COMMAND_LOG"
    grep -Fq -- "-DSLOTNEXUS_ASR_PRECISION=fp32" "$COMMAND_LOG"
    grep -Fq -- "-DSLOTNEXUS_VOICE_FIXTURE_DIR=$VOICE_FIXTURE_DIR" "$COMMAND_LOG"
    grep -Fq -- "-DSLOTNEXUS_RKLLM_MODEL=$RKLLM_MODEL" "$COMMAND_LOG"
    grep -Fq -- "-DSLOTNEXUS_MELOTTS_ENCODER=$MELOTTS_ENCODER" "$COMMAND_LOG"
    grep -Fq -- "-DSLOTNEXUS_MELOTTS_DECODER=$MELOTTS_DECODER" "$COMMAND_LOG"
    grep -Fq -- "-DSLOTNEXUS_MELOTTS_LEXICON=$MELOTTS_LEXICON" "$COMMAND_LOG"
    grep -Fq -- "-DSLOTNEXUS_MELOTTS_TOKENS=$MELOTTS_TOKENS" "$COMMAND_LOG"
    grep -Fq -- "-DSLOTNEXUS_MELOTTS_G=$MELOTTS_G" "$COMMAND_LOG"
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
      -u SLOTNEXUS_SHERTA_ROOT \
      -u SLOTNEXUS_RKLLM_ROOT \
      -u SLOTNEXUS_MELOTTS_ROOT \
      -u SLOTNEXUS_ASR_MODEL \
      -u SLOTNEXUS_RKLLM_MODEL \
      -u SLOTNEXUS_MELOTTS_ENCODER \
      -u SLOTNEXUS_MELOTTS_DECODER \
      -u SLOTNEXUS_MELOTTS_LEXICON \
      -u SLOTNEXUS_MELOTTS_TOKENS \
      -u SLOTNEXUS_MELOTTS_G \
      -u SLOTNEXUS_VOICE_FIXTURE_DIR \
      /bin/bash "$BUILD_SCRIPT" hardware 2>&1)
    STATUS=$?
    set -e
    printf '%s\n' "$OUTPUT"
    if [ "$STATUS" -eq 0 ]; then
      echo '缺失硬件依赖时构建脚本错误返回成功' >&2
      exit 1
    fi
    grep -Fq '缺少硬件构建参数 SLOTNEXUS_SHERTA_ROOT' <<< "$OUTPUT"
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
