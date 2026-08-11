#!/bin/bash
set -euo pipefail

CHECK_SCRIPT=$1
CXX=$2
TEST_ROOT=$(mktemp -d /tmp/voxorchestra-no-hw-deps-test.XXXXXX)
trap 'rm -rf "$TEST_ROOT"' EXIT

BUILD_DIR="$TEST_ROOT/build"
for app in echo_node asr_node rag_node llm_node tts_node unit_manager edge_gateway session_node; do
  mkdir -p "$BUILD_DIR/apps/$app"
  cp /bin/true "$BUILD_DIR/apps/$app/$app"
done

mkdir -p "$BUILD_DIR/apps/voice_cli" "$TEST_ROOT/lib"
printf '%s\n' 'extern "C" int rkllm_probe() { return 7; }' > "$TEST_ROOT/probe.cpp"
"$CXX" -shared -fPIC "$TEST_ROOT/probe.cpp" -o "$TEST_ROOT/lib/librkllm_probe.so"

printf '%s\n' \
  'extern "C" int rkllm_probe();' \
  'int main() { return rkllm_probe() == 7 ? 0 : 1; }' \
  > "$TEST_ROOT/voice_cli.cpp"
"$CXX" "$TEST_ROOT/voice_cli.cpp" \
  -L"$TEST_ROOT/lib" -lrkllm_probe \
  -Wl,-rpath,"$TEST_ROOT/lib" \
  -o "$BUILD_DIR/apps/voice_cli/voice_cli"

set +e
OUTPUT=$(bash "$CHECK_SCRIPT" "$BUILD_DIR" 2>&1)
STATUS=$?
set -e
printf '%s\n' "$OUTPUT"

if [ "$STATUS" -eq 0 ]; then
  echo "检查脚本漏检了 voice_cli 的硬件依赖" >&2
  exit 1
fi

grep -q 'voice_cli: 发现硬件依赖' <<< "$OUTPUT"
