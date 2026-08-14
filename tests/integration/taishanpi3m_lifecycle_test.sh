#!/bin/bash
set -euo pipefail

CASE_NAME=$1
START_SCRIPT=$2
STOP_SCRIPT=$3
TEST_ROOT=$(mktemp -d /tmp/voxorchestra-lifecycle-test.XXXXXX)
RUN_DIR="$TEST_ROOT/run"
SERVICES=(edge_gateway unit_manager session_node asr_node llm_node tts_node)
PIDS=()

cleanup() {
  trap - EXIT
  if [ "${#PIDS[@]}" -gt 0 ]; then
    kill -9 "${PIDS[@]}" 2>/dev/null || true
    wait "${PIDS[@]}" 2>/dev/null || true
  fi
  for service in "${SERVICES[@]}"; do
    pkill -9 -x "$service" 2>/dev/null || true
  done
  rm -rf "$TEST_ROOT"
}
trap cleanup EXIT

mkdir -p "$RUN_DIR"
cat > "$TEST_ROOT/service_stub.cpp" <<'EOF'
#include <libgen.h>
#include <signal.h>
#include <sys/prctl.h>
#include <unistd.h>

volatile sig_atomic_t running = 1;

void stop(int) { running = 0; }

int main(int argc, char** argv) {
  (void)argc;
  const char* name = basename(argv[0]);
  prctl(PR_SET_NAME, name, 0, 0, 0);
  signal(SIGTERM, stop);
  while (running) pause();
  return 0;
}
EOF
g++ -std=c++17 "$TEST_ROOT/service_stub.cpp" -o "$TEST_ROOT/service_stub"

start_service_stubs() {
  for service in "${SERVICES[@]}"; do
    cp "$TEST_ROOT/service_stub" "$TEST_ROOT/$service"
    "$TEST_ROOT/$service" &
    pid=$!
    PIDS+=("$pid")
    echo "$pid" > "$RUN_DIR/$service.pid"
  done

  for index in "${!SERVICES[@]}"; do
    service=${SERVICES[$index]}
    pid=${PIDS[$index]}
    for _ in $(seq 1 20); do
      if [ -r "/proc/$pid/comm" ] && [ "$(cat "/proc/$pid/comm")" = "$service" ]; then
        break
      fi
      sleep 0.05
    done
    [ "$(cat "/proc/$pid/comm")" = "$service" ]
  done
}

assert_services_stopped() {
  for index in "${!SERVICES[@]}"; do
    service=${SERVICES[$index]}
    pid=${PIDS[$index]}
    wait "$pid" 2>/dev/null || true
    if kill -0 "$pid" 2>/dev/null; then
      echo "$service 未停止" >&2
      exit 1
    fi
    if [ -e "$RUN_DIR/$service.pid" ]; then
      echo "$service PID 文件未删除" >&2
      exit 1
    fi
  done
}

prepare_deploy_fixture() {
  BUILD_DIR="$TEST_ROOT/build-taishanpi3m-hw"
  CONFIG="$TEST_ROOT/config/taishanpi3m/session.json"
  RKLLM_ROOT="$TEST_ROOT/sdk/rkllm"
  SHERPA_ROOT="$TEST_ROOT/sdk/sherpa"

  mkdir -p \
    "$TEST_ROOT/config/taishanpi3m" \
    "$TEST_ROOT/data/fixtures" \
    "$TEST_ROOT/data/knowledge" \
    "$TEST_ROOT/models/asr" \
    "$TEST_ROOT/scripts" \
    "$RKLLM_ROOT/aarch64" \
    "$SHERPA_ROOT/build/lib" \
    "$SHERPA_ROOT/build/_deps/onnxruntime-src/lib"

  for service in "${SERVICES[@]}"; do
    mkdir -p "$BUILD_DIR/apps/$service"
    cp "$TEST_ROOT/service_stub" "$BUILD_DIR/apps/$service/$service"
  done

  touch \
    "$TEST_ROOT/data/knowledge/knowledge.jsonl" \
    "$TEST_ROOT/models/asr/tokens.txt" \
    "$TEST_ROOT/models/model.rkllm" \
    "$TEST_ROOT/models/tts.bin" \
    "$RKLLM_ROOT/aarch64/librkllmrt.so" \
    "$SHERPA_ROOT/build/lib/libsherpa-onnx-c-api.so"

  cat > "$CONFIG" <<'EOF'
{
  "knowledge": "data/knowledge/knowledge.jsonl",
  "asr": {"backend": "sherpa_onnx", "model": "models/asr"},
  "llm": {"backend": "rkllm", "model": "models/model.rkllm"},
  "tts": {"backend": "summertts", "model": "models/tts.bin"}
}
EOF

  cat > "$TEST_ROOT/scripts/gateway_probe.py" <<'EOF'
#!/usr/bin/env python3
import os

if os.environ.get("SETUP_FAILURE") == "1":
    print('{"version":1,"type":"error","request_id":"deploy-setup","code":8}')
else:
    print('{"version":1,"type":"ack","request_id":"deploy-setup","work_id":"w-0"}')
EOF
}

run_start() {
  VOXORCHESTRA_DEPLOY_ROOT="$TEST_ROOT" \
  VOXORCHESTRA_BUILD_DIR="$BUILD_DIR" \
  VOXORCHESTRA_CONFIG="$CONFIG" \
  VOXORCHESTRA_RUN_DIR="$RUN_DIR" \
  VOXORCHESTRA_RKLLM_ROOT="$RKLLM_ROOT" \
  VOXORCHESTRA_SHERTA_ROOT="$SHERPA_ROOT" \
  VOXORCHESTRA_SETUP_TIMEOUT_SECONDS=1 \
    /bin/bash "$START_SCRIPT"
}

assert_no_service_processes() {
  for service in "${SERVICES[@]}"; do
    if pgrep -x "$service" >/dev/null; then
      echo "$service 存在残留进程" >&2
      exit 1
    fi
    if [ -e "$RUN_DIR/$service.pid" ]; then
      echo "$service PID 文件残留" >&2
      exit 1
    fi
  done
}

case "$CASE_NAME" in
  stop_idempotent)
    start_service_stubs
    VOXORCHESTRA_RUN_DIR="$RUN_DIR" /bin/bash "$STOP_SCRIPT"
    assert_services_stopped
    VOXORCHESTRA_RUN_DIR="$RUN_DIR" /bin/bash "$STOP_SCRIPT"
    ;;
  stop_force)
    start_service_stubs
    VOXORCHESTRA_RUN_DIR="$RUN_DIR" /bin/bash "$STOP_SCRIPT" --force
    assert_services_stopped
    ;;
  start_success)
    prepare_deploy_fixture
    run_start
    grep -Fq '"type":"ack"' "$RUN_DIR/setup.log"

    for service in "${SERVICES[@]}"; do
      pid=$(cat "$RUN_DIR/$service.pid")
      PIDS+=("$pid")
      kill -0 "$pid"
      [ "$(cat "/proc/$pid/comm")" = "$service" ]
    done

    VOXORCHESTRA_RUN_DIR="$RUN_DIR" /bin/bash "$STOP_SCRIPT"
    assert_services_stopped
    ;;
  missing_runtime)
    prepare_deploy_fixture
    set +e
    OUTPUT=$(env \
      -u VOXORCHESTRA_RKLLM_ROOT \
      VOXORCHESTRA_DEPLOY_ROOT="$TEST_ROOT" \
      VOXORCHESTRA_BUILD_DIR="$BUILD_DIR" \
      VOXORCHESTRA_CONFIG="$CONFIG" \
      VOXORCHESTRA_RUN_DIR="$RUN_DIR" \
      VOXORCHESTRA_SHERTA_ROOT="$SHERPA_ROOT" \
      VOXORCHESTRA_SETUP_TIMEOUT_SECONDS=1 \
        /bin/bash "$START_SCRIPT" 2>&1)
    STATUS=$?
    set -e
    [ "$STATUS" -ne 0 ]
    grep -Fq '缺少运行参数 VOXORCHESTRA_RKLLM_ROOT' <<< "$OUTPUT"
    assert_no_service_processes
    ;;
  setup_failure)
    prepare_deploy_fixture
    export SETUP_FAILURE=1
    set +e
    OUTPUT=$(run_start 2>&1)
    STATUS=$?
    set -e
    [ "$STATUS" -ne 0 ]
    grep -Fq '"type":"error"' "$RUN_DIR/setup.log"
    assert_no_service_processes
    ;;
  *)
    echo "未知测试场景: $CASE_NAME" >&2
    exit 2
    ;;
esac
