#!/bin/bash
# Author: Caden
# 泰山派 3M 语音链路阶段基线：六进程启动/模型 setup、固定 WAV 与真实 LLM
# 轮次的阶段耗时、RSS 与温度。默认不注入人工阶段等待；真实扬声器首音需
# 录音或回环另行测量，本脚本只记录软件提交时间并明确标注未测。
#
# 用法：bash run_baseline.sh [输出目录]
# 依赖根与固定 WAV 路径可用环境变量覆盖（同 run_real_wav_chain.sh）。
# 原始输出：<输出目录>/setup.json、inference_*.json、timings.csv、rss.csv、
#           summary.txt 与六进程日志。
#
# code_version.txt：源码与构建指纹。板端部署目录通常不带 .git，因此按
# Git commit（可用时）+ 源码内容哈希 + 各服务可执行文件哈希记录，保证
# “这组数字出自哪份代码”可核对，而不是只写版本号。
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DEPLOY_ROOT=${SLOTNEXUS_DEPLOY_ROOT:-$(cd "$SCRIPT_DIR/../../../.." && pwd)}
FIXTURE_DIR=${SLOTNEXUS_VOICE_FIXTURE_DIR:-$DEPLOY_ROOT/data/fixtures}
RKLLM_ROOT=${SLOTNEXUS_RKLLM_ROOT:-$HOME/workspace/upstream_rkllm/rknn-llm/rkllm-runtime/Linux/librkllm_api}
SHERTA_ROOT=${SLOTNEXUS_SHERTA_ROOT:-$HOME/workspace/upstream_rkllm/sherpa-root}
MELOTTS_ROOT=${SLOTNEXUS_MELOTTS_ROOT:-$HOME/workspace/upstream_melotts}
ASR_MODEL=${SLOTNEXUS_ASR_MODEL:-$DEPLOY_ROOT/models/sherpa-zipformer-bilingual-zh-en-2023-02-16}
OFFICIAL_WAV=${OFFICIAL_WAV:-$ASR_MODEL/test_wavs/0.wav}
BUILD_DIR=${SLOTNEXUS_BUILD_DIR:-$DEPLOY_ROOT/build-taishanpi3m-hw}
CONFIG=${SLOTNEXUS_CONFIG:-$DEPLOY_ROOT/modules/voice/config/taishanpi3m/session.json}
OUT=${1:-/tmp/slotnexus-baseline}
SERVICES=(edge_gateway unit_manager session_node asr_node llm_node tts_node)

cd "$DEPLOY_ROOT" || exit 1
export LD_LIBRARY_PATH="$RKLLM_ROOT/aarch64:$SHERTA_ROOT/build/lib:$SHERTA_ROOT/build/_deps/onnxruntime-src/lib:$MELOTTS_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
rm -rf "$OUT"
mkdir -p "$OUT/session-out" "$OUT/tts-node"

stop_services() {
  for p in "${SERVICES[@]}"; do pkill -TERM -x "$p" 2>/dev/null; done
  sleep 0.5
}
cleanup() {
  local status=$?
  trap - EXIT INT TERM
  stop_services >/dev/null 2>&1 || true
  exit "$status"
}
trap cleanup EXIT INT TERM
stop_services

record_code_version() {
  local commit="unavailable(no .git)"
  if git -C "$DEPLOY_ROOT" rev-parse HEAD >/dev/null 2>&1; then
    commit=$(git -C "$DEPLOY_ROOT" rev-parse HEAD)
  fi
  # 源码内容哈希：只统计仓库维护的源，排除模型/构建产物/数据。
  local source_hash
  source_hash=$(find "$DEPLOY_ROOT/middleware" "$DEPLOY_ROOT/modules" \
    "$DEPLOY_ROOT/scripts" -type f \
    \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.txt' \
       -o -name '*.sh' -o -name '*.py' -o -name '*.json' -o -name '*.in' \) \
    -exec sha256sum {} + 2>/dev/null | sha256sum | cut -d' ' -f1)
  {
    echo "git_commit=$commit"
    echo "source_sha256=$source_hash"
    echo "build_dir=$BUILD_DIR"
    for name in "${SERVICES[@]}"; do
      local binary="$BUILD_DIR/apps/$name/$name"
      if [ -x "$binary" ]; then
        echo "binary_${name}_sha256=$(sha256sum "$binary" | cut -d' ' -f1)"
      else
        echo "binary_${name}_sha256=missing"
      fi
    done
    echo "config_sha256=$(sha256sum "$CONFIG" 2>/dev/null | cut -d' ' -f1)"
    echo "stage_delay_ms=0"
    echo "captured_at=$(date -Is)"
  } >"$OUT/code_version.txt"
}

record_code_version

start_service() {
  local name=$1
  shift
  nohup "$@" >"$OUT/$name.log" 2>&1 </dev/null &
  echo $! >"$OUT/$name.pid"
}

T_LAUNCH_START=$(date +%s%N)
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
  --output-dir "$OUT/tts-node" \
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
  --net-setup-timeout-ms 60000 --net-rpc-timeout-ms 120000 \
  --config "$CONFIG" --output-dir "$OUT/session-out" \
  --fixture-dir "$FIXTURE_DIR" --sink wav --stage-delay-ms 0
start_service unit_manager "$BUILD_DIR/apps/unit_manager/unit_manager" \
  --module-id voice --default-module voice \
  --node tcp://127.0.0.1:19310 --node-rpc-timeout-ms 120000
start_service edge_gateway "$BUILD_DIR/apps/edge_gateway/edge_gateway" \
  --forward-timeout-ms 120000
T_ALL_SPAWNED=$(date +%s%N)

sleep 2
for name in "${SERVICES[@]}"; do
  pid=$(cat "$OUT/$name.pid")
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "启动失败：$name 已退出，日志见 $OUT/$name.log" >&2
    exit 1
  fi
done

T_GATEWAY_WAIT=$(date +%s%N)
SETUP=$(python3 scripts/gateway_probe.py 9100 \
  '{"version":1,"type":"setup","request_id":"baseline-setup"}' 150)
T_SETUP_DONE=$(date +%s%N)
printf '%s\n' "$SETUP" >"$OUT/setup.json"

python3 - "$SETUP" "$T_LAUNCH_START" "$T_ALL_SPAWNED" "$T_GATEWAY_WAIT" \
  "$T_SETUP_DONE" "$OUT" >"$OUT/startup.csv" <<'PY'
import json
import os
import re
import sys

raw, t0, t_all, t_wait, t_setup, out_dir = sys.argv[1:]
reply = json.loads(raw)
if reply.get("type") != "ack":
    print("setup 失败: %s" % reply, file=sys.stderr)
    raise SystemExit(1)
p = reply.get("payload", {})


def ms_value(line, key="ms"):
    match = re.search(r"%s=(\d+)" % re.escape(key), line)
    return int(match.group(1)) if match else -1


def parse_runtime_setup(path):
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                if "runtime setup" in line:
                    return {
                        "backend_init": ms_value(line, "backend_init_ms"),
                        "task_setup": ms_value(line, "task_setup_ms"),
                        "total": ms_value(line, "total_ms"),
                    }
    except OSError:
        pass
    return {"backend_init": -1, "task_setup": -1, "total": -1}


def parse_net_lines(path, marker):
    values = []
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                if marker in line:
                    values.append(ms_value(line, "ms"))
    except OSError:
        pass
    while len(values) < 3:
        values.append(-1)
    return values[:3]


asr = parse_runtime_setup(os.path.join(out_dir, "asr_node.log"))
llm = parse_runtime_setup(os.path.join(out_dir, "llm_node.log"))
tts = parse_runtime_setup(os.path.join(out_dir, "tts_node.log"))
session_log = os.path.join(out_dir, "session_node.log")
handshakes = parse_net_lines(session_log, "net handshake")
net_setups = parse_net_lines(session_log, "net setup")

print("field,value")
print("spawn_processes_ms,%d" % ((int(t_all) - int(t0)) // 1000000))
print("wait_exit_before_probe_ms,%d" % ((int(t_wait) - int(t_all)) // 1000000))
print("setup_gateway_wall_ms,%d" % ((int(t_setup) - int(t_wait)) // 1000000))
print("ready_total_ms,%d" % ((int(t_setup) - int(t0)) // 1000000))
print("session_setup_ms,%s" % p.get("setup_ms", -1))
# session 记录的是累计 setup 耗时：llm/tts 单节点耗时为相邻差。
asr_ms = int(p.get("asr_setup_ms", 0))
llm_cum = int(p.get("llm_setup_ms", 0))
tts_cum = int(p.get("tts_setup_ms", 0))
print("asr_setup_session_ms,%s" % p.get("asr_setup_ms", -1))
print("llm_setup_session_ms,%s" % (llm_cum - asr_ms))
print("tts_setup_session_ms,%s" % (tts_cum - llm_cum))
print("session_setup_cumulative_ms,%s" % p.get("setup_ms", -1))
print("asr_handshake_ms,%d" % handshakes[0])
print("llm_handshake_ms,%d" % handshakes[1])
print("tts_handshake_ms,%d" % handshakes[2])
print("asr_net_setup_ms,%d" % net_setups[0])
print("llm_net_setup_ms,%d" % net_setups[1])
print("tts_net_setup_ms,%d" % net_setups[2])
print("asr_backend_init_ms,%d" % asr["backend_init"])
print("llm_backend_init_ms,%d" % llm["backend_init"])
print("tts_backend_init_ms,%d" % tts["backend_init"])
print("asr_task_setup_ms,%d" % asr["task_setup"])
print("llm_task_setup_ms,%d" % llm["task_setup"])
print("tts_task_setup_ms,%d" % tts["task_setup"])
PY
SETUP_RC=$?
if [ "$SETUP_RC" -ne 0 ]; then
  echo "setup 未通过，保留日志：$OUT" >&2
  cat "$OUT/setup.json" >&2
  exit "$SETUP_RC"
fi

WORK_ID=$(python3 - "$SETUP" <<'PY'
import json
import sys
print(json.loads(sys.argv[1]).get("work_id", ""))
PY
)
if [ -z "$WORK_ID" ]; then
  echo "setup 未返回 work_id" >&2
  exit 1
fi

run_inference() {
  local label=$1
  local payload=$2
  local timeout=$3
  local t_start t_end
  t_start=$(date +%s%N)
  local raw
  raw=$(python3 scripts/gateway_probe.py 9100 "$payload" "$timeout")
  local rc=$?
  t_end=$(date +%s%N)
  printf '%s\n' "$raw" >"$OUT/inference_$label.json"
  if [ "$rc" -ne 0 ]; then
    echo "inference $label 失败（exit=$rc）" >&2
    return 1
  fi
  python3 - "$label" "$raw" "$t_start" "$t_end" "$OUT/timings.csv" <<'PY'
import json
import os
import sys

label, raw, t_start, t_end, csv_path = sys.argv[1:]
reply = json.loads(raw)
p = reply.get("payload", {})
t = p.get("timings_ms", {})
if reply.get("type") != "ack" or p.get("status") != "ok":
    print("inference %s 未通过: type=%s status=%s error=%s" %
          (label, reply.get("type"), p.get("status"), p.get("error")),
          file=sys.stderr)
    raise SystemExit(1)
row = {
    "label": label,
    "wall_ms": (int(t_end) - int(t_start)) // 1000000,
    "route": p.get("route", ""),
    "status": p.get("status", ""),
    "token_count": p.get("token_count", 0),
    "pcm_frames": p.get("pcm_frames", 0),
    "input_end_ms": t.get("input_end", -1),
    "asr_final_ms": t.get("asr_final", -1),
    "llm_first_token_ms": t.get("llm_first_token", -1),
    "first_text_ms": t.get("first_text", -1),
    "tts_first_pcm_ms": t.get("tts_first_pcm", -1),
    "first_output_ms": t.get("first_output", -1),
    "output_complete_ms": t.get("output_complete", -1),
    "total_ms": t.get("total", -1),
    "output_mode": p.get("output_mode", ""),
    "audio_delivered": p.get("audio_delivered", False),
    "wav_complete": p.get("wav_complete", False),
    "playback_complete": p.get("playback_complete", False),
    "acoustic_first_sound": "not_measured(wav/no_loopback)",
}
fields = [
    "label", "wall_ms", "route", "status", "token_count", "pcm_frames",
    "input_end_ms", "asr_final_ms", "llm_first_token_ms", "first_text_ms",
    "tts_first_pcm_ms", "first_output_ms", "output_complete_ms", "total_ms",
    "output_mode", "audio_delivered", "wav_complete", "playback_complete",
    "acoustic_first_sound",
]
new_file = not os.path.exists(csv_path)
with open(csv_path, "a", encoding="utf-8") as f:
    if new_file:
        f.write(",".join(fields) + "\n")
    f.write(",".join(str(row[f]) for f in fields) + "\n")
PY
}

report_resources() {
  local label=$1
  python3 - "$label" "$OUT/rss.csv" <<'PY'
import os
import subprocess
import sys
import time

label, csv_path = sys.argv[1:]
services = ["asr_node", "llm_node", "tts_node", "session_node",
            "edge_gateway", "unit_manager"]
values = {}
for name in services:
    total = 0
    out = subprocess.run(["pgrep", "-x", name], capture_output=True, text=True)
    for pid in out.stdout.split():
        try:
            with open("/proc/%s/status" % pid) as f:
                for line in f:
                    if line.startswith("VmRSS:"):
                        total += int(line.split()[1])
        except OSError:
            pass
    values[name + "_rss_kb"] = total


def temp_c():
    for zone in ("/sys/class/thermal/thermal_zone0",
                 "/sys/class/thermal/thermal_zone1"):
        try:
            with open(zone + "/temp") as f:
                return int(f.read().strip()) / 1000.0
        except OSError:
            pass
    return ""


values["temp_c"] = temp_c()
fields = ["label", "asr_node_rss_kb", "llm_node_rss_kb", "tts_node_rss_kb",
          "session_node_rss_kb", "edge_gateway_rss_kb",
          "unit_manager_rss_kb", "temp_c"]
new_file = not os.path.exists(csv_path)
with open(csv_path, "a", encoding="utf-8") as f:
    if new_file:
        f.write(",".join(fields) + "\n")
    f.write(str(label) + "," + ",".join(str(values.get(k, "")) for k in fields[1:]) + "\n")
PY
}

report_resources "after_setup"
run_inference "demo_zh" \
  "{\"version\":1,\"type\":\"inference\",\"work_id\":\"$WORK_ID\",\"request_id\":\"baseline-demo\",\"payload\":{\"mode\":\"wav\",\"wav\":\"demo_zh.wav\"}}" \
  180 || exit 1
report_resources "after_demo_zh"

if [ ! -f "$OFFICIAL_WAV" ]; then
  echo "缺少真实 LLM 固定 WAV: $OFFICIAL_WAV；demo_zh 结果保留，L3 标为未测" >&2
  exit 2
fi
run_inference "official_llm" \
  "{\"version\":1,\"type\":\"inference\",\"work_id\":\"$WORK_ID\",\"request_id\":\"baseline-official\",\"payload\":{\"mode\":\"wav\",\"wav\":\"$OFFICIAL_WAV\"}}" \
  240 || exit 1
report_resources "after_official_llm"

python3 - "$OUT" >"$OUT/summary.txt" <<'PY'
import csv
import os
import sys

out = sys.argv[1]
print("SlotNexus 泰山派语音阶段基线")
print("acoustic_first_sound=not_measured：WAV 模式仅记录 sink 提交/文件完成，未接录音或回环")
print()
print("== 代码版本 ==")
version_path = os.path.join(out, "code_version.txt")
if os.path.exists(version_path):
    with open(version_path, encoding="utf-8") as f:
        for line in f:
            print("  " + line.rstrip())
print()
print("== 六进程启动与 setup ==")
with open(os.path.join(out, "startup.csv"), encoding="utf-8") as f:
    for row in csv.reader(f):
        print("  " + "=".join(row))
print()
print("== 推理阶段 ==")
with open(os.path.join(out, "timings.csv"), encoding="utf-8") as f:
    for row in csv.DictReader(f):
        print("  %(label)s wall=%(wall_ms)sms route=%(route)s tokens=%(token_count)s "
              "pcm=%(pcm_frames)s input_end=%(input_end_ms)s asr_final=%(asr_final_ms)s "
              "llm_first=%(llm_first_token_ms)s first_text=%(first_text_ms)s "
              "tts_first_pcm=%(tts_first_pcm_ms)s first_output=%(first_output_ms)s "
              "output_complete=%(output_complete_ms)s total=%(total_ms)s "
              "wav_complete=%(wav_complete)s playback_complete=%(playback_complete)s" % row)
print()
print("== RSS/温度 ==")
with open(os.path.join(out, "rss.csv"), encoding="utf-8") as f:
    for row in f:
        print("  " + row.rstrip())
PY

echo "基线原始数据目录: $OUT"
echo "代码版本与构建指纹: $OUT/code_version.txt"
exit 0
