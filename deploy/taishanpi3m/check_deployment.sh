#!/bin/bash
# 泰山派 3M 全真实链路启动前的只读部署预检。
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DEFAULT_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
ROOT=${VOXORCHESTRA_DEPLOY_ROOT:-$DEFAULT_ROOT}
BUILD_DIR=${VOXORCHESTRA_BUILD_DIR:-$ROOT/build-taishanpi3m-hw}
CONFIG=${VOXORCHESTRA_CONFIG:-$ROOT/config/taishanpi3m/session.json}

fail() {
  echo "部署预检失败: $*" >&2
  exit 1
}

for command_name in python3 ldd; do
  command -v "$command_name" >/dev/null || fail "缺少命令 $command_name"
done

[ -f "$CONFIG" ] || fail "缺少板端配置: $CONFIG"

for app in edge_gateway unit_manager session_node asr_node llm_node tts_node; do
  binary="$BUILD_DIR/apps/$app/$app"
  [ -x "$binary" ] || fail "缺少可执行程序 $app: $binary"

  if ! dependencies=$(ldd "$binary" 2>&1); then
    fail "无法读取 $app 的动态库依赖"
  fi
  if grep -Fq 'not found' <<< "$dependencies"; then
    echo "$dependencies" >&2
    fail "$app 存在未解析动态库"
  fi
done

python3 - "$ROOT" "$CONFIG" <<'PY'
import json
import os
import sys

root, config_path = sys.argv[1:]

try:
    with open(config_path, "r", encoding="utf-8") as config_file:
        config = json.load(config_file)
except (OSError, json.JSONDecodeError) as error:
    print(f"部署预检失败: 无法解析板端配置: {error}", file=sys.stderr)
    raise SystemExit(1)

expected_backends = {
    "asr": "sherpa_onnx",
    "llm": "rkllm",
    "tts": "summertts",
}
for section, expected in expected_backends.items():
    actual = config.get(section, {}).get("backend")
    if actual != expected:
        print(
            f"部署预检失败: {section.upper()} Backend 应为 {expected}，实际为 {actual!r}",
            file=sys.stderr,
        )
        raise SystemExit(1)

path_specs = (
    ("知识库", config.get("knowledge"), "file"),
    ("ASR 模型", config.get("asr", {}).get("model"), "directory"),
    ("LLM 模型", config.get("llm", {}).get("model"), "file"),
    ("TTS 模型", config.get("tts", {}).get("model"), "file"),
)

for label, configured_path, expected_type in path_specs:
    if not isinstance(configured_path, str) or not configured_path:
        print(f"部署预检失败: 配置缺少 {label}路径", file=sys.stderr)
        raise SystemExit(1)

    resolved_path = configured_path
    if not os.path.isabs(resolved_path):
        resolved_path = os.path.join(root, resolved_path)

    exists = os.path.isdir(resolved_path) if expected_type == "directory" else os.path.isfile(resolved_path)
    if not exists:
        print(f"部署预检失败: 缺少 {label}: {configured_path}", file=sys.stderr)
        raise SystemExit(1)

    print(f"{label}: {configured_path}")
PY

echo "部署预检通过: 六个程序、板端配置、模型与动态库均已就绪"
