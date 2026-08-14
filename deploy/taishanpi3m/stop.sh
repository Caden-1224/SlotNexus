#!/bin/bash
# 停止泰山派 3M 全真实链路的六个后台服务。
set -euo pipefail

SERVICES=(edge_gateway unit_manager session_node asr_node llm_node tts_node)
RUN_DIR=${VOXORCHESTRA_RUN_DIR:-/tmp/voxorchestra-runtime}
MODE=${1:-graceful}

if [ "$MODE" != graceful ] && [ "$MODE" != --force ]; then
  echo "用法: bash deploy/taishanpi3m/stop.sh [--force]" >&2
  exit 2
fi

pid_matches_service() {
  local pid=$1
  local service=$2
  [ -r "/proc/$pid/comm" ] && [ "$(cat "/proc/$pid/comm")" = "$service" ]
}

pid_is_running_service() {
  local pid=$1
  local service=$2
  local state
  pid_matches_service "$pid" "$service" || return 1
  state=$(awk '{print $3}' "/proc/$pid/stat" 2>/dev/null) || return 1
  [ "$state" != Z ]
}

read_service_pid() {
  local service=$1
  local pid_file="$RUN_DIR/$service.pid"
  local pid
  [ -r "$pid_file" ] || return 1
  pid=$(cat "$pid_file")
  [[ "$pid" =~ ^[1-9][0-9]*$ ]] || return 1
  printf '%s\n' "$pid"
}

force_cleanup() {
  local service
  for service in "${SERVICES[@]}"; do
    pkill -9 -x "$service" 2>/dev/null || true
    rm -f "$RUN_DIR/$service.pid"
  done
}

if [ "$MODE" = --force ]; then
  force_cleanup
  echo "六个服务已强制清理"
  exit 0
fi

for service in "${SERVICES[@]}"; do
  if pid=$(read_service_pid "$service") && pid_matches_service "$pid" "$service"; then
    kill -TERM "$pid" 2>/dev/null || true
  fi
done

for ((attempt = 0; attempt < 40; ++attempt)); do
  alive=0
  for service in "${SERVICES[@]}"; do
    if pid=$(read_service_pid "$service") && pid_is_running_service "$pid" "$service"; then
      alive=1
      break
    fi
  done
  [ "$alive" -eq 0 ] && break
  sleep 0.5
done

for service in "${SERVICES[@]}"; do
  if pid=$(read_service_pid "$service") && pid_is_running_service "$pid" "$service"; then
    kill -9 "$pid" 2>/dev/null || true
  fi
done

force_cleanup
echo "六个服务已停止"
