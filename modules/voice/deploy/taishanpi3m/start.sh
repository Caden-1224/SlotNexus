#!/bin/bash
# Author: Caden
# 启动泰山派 3M 全真实链路的六个后台服务并完成模型 setup。
# 进程参数与退出逻辑见 common.sh / stop.sh；本脚本只负责发布前的参数校验、
# 回滚与 setup 确认。
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
source "$SCRIPT_DIR/common.sh"
load_environment

SETUP_TIMEOUT=${SLOTNEXUS_SETUP_TIMEOUT_SECONDS:-120}

fail() {
  echo "启动失败: $*" >&2
  return 1
}

require_parameter() {
  local name=$1
  [ -n "${!name:-}" ] || fail "缺少运行参数 $name"
}

require_path() {
  local path=$1
  local description=$2
  [ -e "$path" ] || fail "缺少$description: $path"
}

bash "$SCRIPT_DIR/stop.sh" --force
mkdir -p "$RUN_DIR"

rollback() {
  local status=$?
  trap - EXIT INT TERM
  echo "启动未完成，正在清理六个服务；日志保留在 $RUN_DIR" >&2
  SLOTNEXUS_RUN_DIR="$RUN_DIR" bash "$SCRIPT_DIR/stop.sh" --force >/dev/null 2>&1 || true
  exit "$status"
}
trap rollback EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

require_parameter SLOTNEXUS_RKLLM_ROOT
require_parameter SLOTNEXUS_SHERTA_ROOT
require_parameter SLOTNEXUS_MELOTTS_ROOT
if ! [[ "$SETUP_TIMEOUT" =~ ^[1-9][0-9]*$ ]]; then
  fail "SLOTNEXUS_SETUP_TIMEOUT_SECONDS 必须是正整数"
fi
require_path "$DEPLOY_ROOT" "部署根目录"

RKLLM_LIB="$RKLLM_ROOT/aarch64"
SHERPA_LIB="$SHERTA_ROOT/build/lib"
ONNX_LIB="$SHERTA_ROOT/build/_deps/onnxruntime-src/lib"
MELOTTS_LIB="$MELOTTS_ROOT/lib"
require_path "$RKLLM_LIB/librkllmrt.so" " RKLLM Runtime"
require_path "$SHERPA_LIB/libsherpa-onnx-c-api.so" " sherpa-onnx 动态库"
require_path "$ONNX_LIB" " ONNX Runtime 动态库目录（sherpa-onnx）"
require_path "$MELOTTS_LIB/libonnxruntime.so" " ONNX Runtime 动态库（MeloTTS）"
require_path "$MELOTTS_LIB/librknnrt.so" " RKNN Runtime（MeloTTS 解码器）"
command -v nohup >/dev/null || fail "缺少命令 nohup"
command -v python3 >/dev/null || fail "缺少命令 python3"

# LD_LIBRARY_PATH 由 load_environment 按依赖根拼好；这里只把位置信息传给
# 子进程（check_deployment.sh 与各节点按同一组变量定位资产）。
export SLOTNEXUS_DEPLOY_ROOT="$DEPLOY_ROOT"
export SLOTNEXUS_BUILD_DIR="$BUILD_DIR"
export SLOTNEXUS_CONFIG="$CONFIG"
export SLOTNEXUS_RUN_DIR="$RUN_DIR"

cd "$DEPLOY_ROOT"
bash "$SCRIPT_DIR/check_deployment.sh"
mkdir -p "$RUN_DIR/session-out" "$RUN_DIR/tts-node"

start_real_chain

sleep 2
for service in "${SERVICES[@]}"; do
  pid=$(cat "$RUN_DIR/$service.pid")
  if ! kill -0 "$pid" 2>/dev/null; then
    fail "$service 在 setup 前退出，日志: $RUN_DIR/$service.log"
  fi
done

SETUP_REPLY=$(python3 "$DEPLOY_ROOT/scripts/gateway_probe.py" 9100 \
  '{"version":1,"type":"setup","request_id":"deploy-setup"}' \
  "$SETUP_TIMEOUT")
printf '%s\n' "$SETUP_REPLY" >"$RUN_DIR/setup.log"
python3 - "$SETUP_REPLY" <<'PY'
import json
import sys

reply = json.loads(sys.argv[1])
if reply.get("type") != "ack":
    raise SystemExit(f"setup 返回非 ack: {reply}")
PY

trap - EXIT INT TERM
echo "全真实链路启动完成；日志目录: $RUN_DIR"
