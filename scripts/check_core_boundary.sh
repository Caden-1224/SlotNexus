#!/usr/bin/env bash
# 中间件核心静态边界门禁：禁止核心源码、公开头和 CMake 反向依赖语音模块
# 或音频类型。
#
# Author: Caden
# 设计意图：在编译前抓出 middleware/ 对 modules/voice、voice target 或
# 16-bit PCM 容器的引用。允许核心通用事件使用不透明 JSON；语音适配器在
# modules/voice 内完成专有语义转换。
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
MIDDLEWARE="$ROOT/middleware"
fail=0

report_matches() {
  local label="$1"
  local output="$2"
  if [[ -n "$output" ]]; then
    echo "FAIL: $label"
    echo "$output"
    fail=1
  else
    echo "ok: $label"
  fi
}

includes="$(grep -RInE '#include[[:space:]]*[<"](slotnexus/backend|slotnexus/voice)' \
  "$MIDDLEWARE" --include='*.hpp' --include='*.cpp' 2>/dev/null || true)"
report_matches "核心源码/公开头不得 include 语音头" "$includes"

cmake_refs="$(grep -RInE 'modules/voice|slotnexus::(voice_|backend)' \
  "$MIDDLEWARE" --include='CMakeLists.txt' 2>/dev/null || true)"
report_matches "核心 CMake 不得链接语音目录或语音 target" "$cmake_refs"

audio_types="$(grep -RInE 'std::vector[[:space:]]*<[[:space:]]*(std::)?int16_t[[:space:]]*>' \
  "$MIDDLEWARE" --include='*.hpp' --include='*.cpp' 2>/dev/null || true)"
report_matches "核心源码/公开头不得出现 int16_t PCM 容器" "$audio_types"

if [[ "$fail" -ne 0 ]]; then
  exit 1
fi
echo "check_core_boundary: 中间件核心无语音/音频类型反向依赖"
