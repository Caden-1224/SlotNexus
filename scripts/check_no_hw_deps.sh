#!/bin/bash
# 验收检查：默认构建的所有可执行文件不得链接任何硬件 Backend 依赖
# （RKLLM / sherpa-onnx / SummerTTS / ALSA 声卡库）。
# 用法：scripts/check_no_hw_deps.sh [build 目录]（默认 build-wsl）
set -u
cd "$(dirname "$0")/.."
BUILD="${1:-build-wsl}"

BINS=(
  "$BUILD/apps/echo_node/echo_node"
  "$BUILD/apps/asr_node/asr_node"
  "$BUILD/apps/rag_node/rag_node"
  "$BUILD/apps/llm_node/llm_node"
  "$BUILD/apps/tts_node/tts_node"
  "$BUILD/apps/unit_manager/unit_manager"
  "$BUILD/apps/edge_gateway/edge_gateway"
  "$BUILD/apps/session_node/session_node"
  "$BUILD/apps/voice_cli/voice_cli"
)
PATTERN='rkllm|rknn|sherpa|onnx|summer|asound|libsndfile'
FAILED=0

for B in "${BINS[@]}"; do
  if [ ! -x "$B" ]; then
    echo "缺失二进制: $B"
    FAILED=1
    continue
  fi
  HIT=$(ldd "$B" | grep -iE "$PATTERN" | head -1)
  if [ -n "$HIT" ]; then
    echo "$(basename "$B"): 发现硬件依赖 -> $HIT"
    FAILED=1
  else
    echo "$(basename "$B"): 无硬件依赖"
  fi
done

if [ "$FAILED" -eq 0 ]; then
  echo "检查通过：默认构建不依赖 NPU SDK 或声卡"
  exit 0
fi
echo "检查失败：存在硬件依赖"
exit 1
