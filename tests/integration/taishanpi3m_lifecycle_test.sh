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
  *)
    echo "未知测试场景: $CASE_NAME" >&2
    exit 2
    ;;
esac
