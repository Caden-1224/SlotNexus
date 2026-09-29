#!/bin/bash
# Author: Caden
# 泰山派 3M 全真实链路启动前的只读部署预检。
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
source "$SCRIPT_DIR/common.sh"
load_environment

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

python3 - "$DEPLOY_ROOT" "$CONFIG" <<'PY'
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

allowed_backends = {
    "asr": ("sherpa_onnx",),
    "llm": ("rkllm",),
    "tts": ("melotts",),
}
for section, allowed in allowed_backends.items():
    actual = config.get(section, {}).get("backend")
    if actual not in allowed:
        print(
            f"部署预检失败: {section.upper()} Backend 应为 {'/'.join(allowed)}，实际为 {actual!r}",
            file=sys.stderr,
        )
        raise SystemExit(1)

asr = config.get("asr", {})
precision = asr.get("model_precision", "fp32")
if precision not in ("fp32", "int8"):
    print(
        f"部署预检失败: asr.model_precision 应为 fp32/int8，实际为 {precision!r}",
        file=sys.stderr,
    )
    raise SystemExit(1)


def resolve(path):
    if not isinstance(path, str) or not path:
        return None
    return path if os.path.isabs(path) else os.path.join(root, path)


tts = config.get("tts", {})
path_specs = [
    ("知识库", config.get("knowledge"), "file"),
    ("ASR 模型目录", asr.get("model"), "directory"),
    ("LLM 模型", config.get("llm", {}).get("model"), "file"),
]
if tts.get("backend") == "melotts":
    path_specs += [
        ("MeloTTS 编码器模型", tts.get("encoder_model"), "file"),
        ("MeloTTS 解码器模型", tts.get("decoder_model"), "file"),
        ("MeloTTS 词典", tts.get("lexicon"), "file"),
        ("MeloTTS token 表", tts.get("tokens"), "file"),
        ("MeloTTS g 向量", tts.get("g_vector"), "file"),
    ]
for label, configured_path, expected_type in path_specs:
    resolved_path = resolve(configured_path)
    if resolved_path is None:
        print(f"部署预检失败: 配置缺少 {label}路径", file=sys.stderr)
        raise SystemExit(1)
    exists = (
        os.path.isdir(resolved_path)
        if expected_type == "directory"
        else os.path.isfile(resolved_path)
    )
    if not exists:
        print(f"部署预检失败: 缺少 {label}: {configured_path}", file=sys.stderr)
        raise SystemExit(1)
    print(f"{label}: {configured_path}")

# ASR 后端按精度选择 fp32 或 int8 的 encoder/decoder/joiner 三元组；
# bpe.model/tokens.txt 必须与模型包同批提供，避免路径或版本链错配。
asr_dir = resolve(asr.get("model"))
suffix = ".int8.onnx" if precision == "int8" else ".onnx"
# 官方 int8 配方固定使用 fp32 decoder；encoder/joiner 按精度切换。
asr_files = [
    f"encoder-epoch-99-avg-1{suffix}",
    "decoder-epoch-99-avg-1.onnx",
    f"joiner-epoch-99-avg-1{suffix}",
    "tokens.txt",
    "bpe.model",
]
for name in asr_files:
    candidate = os.path.join(asr_dir, name)
    if not os.path.isfile(candidate):
        print(
            f"部署预检失败: ASR 精度 {precision} 缺少模型文件 {name}: {candidate}",
            file=sys.stderr,
        )
        raise SystemExit(1)
    print(f"ASR 模型文件: {name}")

print(f"ASR 模型精度: {precision}")
PY

echo "部署预检通过: 六个程序、板端配置、模型与动态库均已就绪"
