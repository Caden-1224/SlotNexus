#!/bin/bash
# 启动泰山派 3M 全真实链路的六个后台服务并完成模型 setup。
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DEFAULT_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
ROOT=${VOXORCHESTRA_DEPLOY_ROOT:-$DEFAULT_ROOT}
BUILD_DIR=${VOXORCHESTRA_BUILD_DIR:-$ROOT/build-taishanpi3m-hw}
CONFIG=${VOXORCHESTRA_CONFIG:-$ROOT/config/taishanpi3m/session.json}
RUN_DIR=${VOXORCHESTRA_RUN_DIR:-/tmp/voxorchestra-runtime}
SETUP_TIMEOUT=${VOXORCHESTRA_SETUP_TIMEOUT_SECONDS:-120}
SERVICES=(edge_gateway unit_manager session_node asr_node llm_node tts_node)

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
  VOXORCHESTRA_RUN_DIR="$RUN_DIR" bash "$SCRIPT_DIR/stop.sh" --force >/dev/null 2>&1 || true
  exit "$status"
}
trap rollback EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

require_parameter VOXORCHESTRA_RKLLM_ROOT
require_parameter VOXORCHESTRA_SHERTA_ROOT
if ! [[ "$SETUP_TIMEOUT" =~ ^[1-9][0-9]*$ ]]; then
  fail "VOXORCHESTRA_SETUP_TIMEOUT_SECONDS 必须是正整数"
fi
require_path "$ROOT" "部署根目录"

RKLLM_LIB="$VOXORCHESTRA_RKLLM_ROOT/aarch64"
SHERPA_LIB="$VOXORCHESTRA_SHERTA_ROOT/build/lib"
ONNX_LIB="$VOXORCHESTRA_SHERTA_ROOT/build/_deps/onnxruntime-src/lib"
require_path "$RKLLM_LIB/librkllmrt.so" " RKLLM Runtime"
require_path "$SHERPA_LIB/libsherpa-onnx-c-api.so" " sherpa-onnx 动态库"
require_path "$ONNX_LIB" " ONNX Runtime 动态库目录"
command -v nohup >/dev/null || fail "缺少命令 nohup"
command -v python3 >/dev/null || fail "缺少命令 python3"

export LD_LIBRARY_PATH="$RKLLM_LIB:$SHERPA_LIB:$ONNX_LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export VOXORCHESTRA_DEPLOY_ROOT="$ROOT"
export VOXORCHESTRA_BUILD_DIR="$BUILD_DIR"
export VOXORCHESTRA_CONFIG="$CONFIG"
export VOXORCHESTRA_RUN_DIR="$RUN_DIR"

cd "$ROOT"
bash "$SCRIPT_DIR/check_deployment.sh"
mkdir -p "$RUN_DIR/session-out" "$RUN_DIR/tts-node"

start_service() {
  local service=$1
  shift
  nohup "$@" >"$RUN_DIR/$service.log" 2>&1 </dev/null &
  local pid=$!
  echo "$pid" >"$RUN_DIR/$service.pid"
  sleep 0.1
  if ! kill -0 "$pid" 2>/dev/null; then
    fail "$service 启动失败，日志: $RUN_DIR/$service.log"
  fi
}

start_service asr_node "$BUILD_DIR/apps/asr_node/asr_node" \
  --listen tcp://127.0.0.1:19201 --config "$CONFIG" \
  --events tcp://127.0.0.1:19421 --events-sync tcp://127.0.0.1:19422 \
  --infer-timeout-ms 30000
start_service llm_node "$BUILD_DIR/apps/llm_node/llm_node" \
  --listen tcp://127.0.0.1:19203 --config "$CONFIG" \
  --events tcp://127.0.0.1:19431 --events-sync tcp://127.0.0.1:19432 \
  --infer-timeout-ms 60000
start_service tts_node "$BUILD_DIR/apps/tts_node/tts_node" \
  --listen tcp://127.0.0.1:19204 --config "$CONFIG" \
  --output-dir "$RUN_DIR/tts-node" \
  --events tcp://127.0.0.1:19441 --events-sync tcp://127.0.0.1:19442 \
  --infer-timeout-ms 30000
start_service session_node "$BUILD_DIR/apps/session_node/session_node" \
  --listen tcp://127.0.0.1:19310 --backend net --asr-uplink \
  --asr-endpoint tcp://127.0.0.1:19201 \
  --asr-events tcp://127.0.0.1:19421 --asr-events-sync tcp://127.0.0.1:19422 \
  --llm-endpoint tcp://127.0.0.1:19203 \
  --llm-events tcp://127.0.0.1:19431 --llm-events-sync tcp://127.0.0.1:19432 \
  --tts-endpoint tcp://127.0.0.1:19204 \
  --tts-events tcp://127.0.0.1:19441 --tts-events-sync tcp://127.0.0.1:19442 \
  --net-setup-timeout-ms 60000 --net-rpc-timeout-ms 60000 \
  --config "$CONFIG" --output-dir "$RUN_DIR/session-out" \
  --fixture-dir "$ROOT/data/fixtures" --stage-delay-ms 20
start_service unit_manager "$BUILD_DIR/apps/unit_manager/unit_manager" \
  --node tcp://127.0.0.1:19310 --node-rpc-timeout-ms 120000
start_service edge_gateway "$BUILD_DIR/apps/edge_gateway/edge_gateway" \
  --forward-timeout-ms 120000

sleep 2
for service in "${SERVICES[@]}"; do
  pid=$(cat "$RUN_DIR/$service.pid")
  if ! kill -0 "$pid" 2>/dev/null; then
    fail "$service 在 setup 前退出，日志: $RUN_DIR/$service.log"
  fi
done

SETUP_REPLY=$(python3 "$ROOT/scripts/gateway_probe.py" 9100 \
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
