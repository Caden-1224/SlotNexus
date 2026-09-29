#!/bin/bash
# Author: Caden
# 板端测量与回归入口：启动基线、固定 WAV、现场麦克风、多轮稳定性、故障注入
# 与单节点核验。六个服务的启动参数、优雅退出、探测与采样集中在 common.sh，
# 场景只描述“测什么、断言什么”。
#
# 用法：bash run.sh <场景> [参数]
#   baseline [输出目录]  六进程启动/模型加载/握手/setup 与推理阶段耗时、
#                        RSS/温度、源码与构建指纹（固定 WAV、零阶段等待）
#   wav                  固定 WAV 全链路回归：demo_zh + 官方 0.wav
#   mic                  现场麦克风闭环（录音时长 SLOTNEXUS_MIC_RECORD_MS、
#                        设备 SLOTNEXUS_MIC_DEVICE，默认 3s / plughw:0,0）
#   stability [轮次]     每轮重建六进程的重复可用性回归（默认 30 轮）
#   inject               取消 / 节点超时 / 错误输入
#   llm                  仅 llm_node 的固定 prompt 核验
#   mock                 Fake Backend 会话链（default 构建产物）
#
# 依赖根、固定 WAV 与输出目标都可用环境变量覆盖，见 common.sh。
# 每个场景的原始输出落在自己的 /tmp 目录，结束时会停止六个服务。
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
source "$SCRIPT_DIR/common.sh"
load_environment
cd "$DEPLOY_ROOT" || exit 1

CHAIN_STARTED=0

# 异常退出也要收尾，否则六个服务会留在板上占用 NPU、端口与内存。
on_exit() {
  local status=$?
  trap - EXIT
  if [ "$status" -ne 0 ] && [ "$CHAIN_STARTED" = 1 ]; then
    reset_services
  fi
  exit "$status"
}
trap on_exit EXIT

usage() {
  cat >&2 <<'EOF'
用法: bash run.sh <场景> [参数]
  baseline [输出目录]  启动与推理阶段基线 + RSS/温度 + 代码指纹
  wav                  固定 WAV 全链路回归
  mic                  现场麦克风闭环
  stability [轮次]     多轮重复可用性回归（默认 30）
  inject               取消 / 超时 / 错误输入
  llm                  仅 llm_node 核验
  mock                 Fake Backend 会话链
EOF
  exit 2
}

# 固定 WAV 推理请求：wav_inference_payload <work_id> <request_id> <wav 路径>
# 用 python 组装 JSON，避免 WAV 路径里的引号或反斜杠破坏请求。
wav_inference_payload() {
  python3 -c '
import json
import sys

print(json.dumps({
    "version": 1,
    "type": "inference",
    "work_id": sys.argv[1],
    "request_id": sys.argv[2],
    "payload": {"mode": "wav", "wav": sys.argv[3]},
}, ensure_ascii=False))
' "$1" "$2" "$3"
}

# ---------------------------------------------------------------- baseline
# 六进程启动、模型加载、节点握手、任务 setup 与推理各阶段耗时、RSS/温度。
scenario_baseline() {
  RUN_DIR=${1:-/tmp/slotnexus-baseline}
  # 基线固定 WAV 输出与零阶段等待：阶段耗时只反映模型与链路。实时播放用
  # start.sh 的 SLOTNEXUS_SINK，人工慢消费用 SLOTNEXUS_STAGE_DELAY_MS。
  SINK=wav
  STAGE_DELAY_MS=0
  rm -rf "$RUN_DIR"
  mkdir -p "$RUN_DIR/session-out" "$RUN_DIR/tts-node"
  reset_services
  record_code_version
  CHAIN_STARTED=1

  local t_launch_start t_all_spawned t_gateway_wait t_setup_done
  t_launch_start=$(date +%s%N)
  start_real_chain
  t_all_spawned=$(date +%s%N)

  sleep 2
  local name
  for name in "${SERVICES[@]}"; do
    if ! kill -0 "$(cat "$RUN_DIR/$name.pid")" 2>/dev/null; then
      echo "启动失败：$name 已退出，日志见 $RUN_DIR/$name.log" >&2
      exit 1
    fi
  done

  t_gateway_wait=$(date +%s%N)
  local setup
  setup=$(probe_gateway '{"version":1,"type":"setup","request_id":"baseline-setup"}' 150)
  t_setup_done=$(date +%s%N)
  printf '%s\n' "$setup" >"$RUN_DIR/setup.json"

  python3 - "$setup" "$t_launch_start" "$t_all_spawned" "$t_gateway_wait" \
    "$t_setup_done" "$RUN_DIR" >"$RUN_DIR/startup.csv" <<'PY'
import json
import os
import re
import sys

raw, t0, t_all, t_wait, t_setup, run_dir = sys.argv[1:]
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


asr = parse_runtime_setup(os.path.join(run_dir, "asr_node.log"))
llm = parse_runtime_setup(os.path.join(run_dir, "llm_node.log"))
tts = parse_runtime_setup(os.path.join(run_dir, "tts_node.log"))
session_log = os.path.join(run_dir, "session_node.log")
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
  local setup_rc=$?
  if [ "$setup_rc" -ne 0 ]; then
    echo "setup 未通过，保留日志：$RUN_DIR" >&2
    cat "$RUN_DIR/setup.json" >&2
    exit "$setup_rc"
  fi

  local work_id
  work_id=$(python3 - "$setup" <<'PY'
import json
import sys

print(json.loads(sys.argv[1]).get("work_id", ""))
PY
)
  if [ -z "$work_id" ]; then
    echo "setup 未返回 work_id" >&2
    exit 1
  fi

  append_resource_row "after_setup" "$RUN_DIR/rss.csv"
  record_inference "demo_zh" "$work_id" 'demo_zh.wav' 180 || exit 1
  append_resource_row "after_demo_zh" "$RUN_DIR/rss.csv"

  if [ ! -f "$OFFICIAL_WAV" ]; then
    echo "缺少真实 LLM 固定 WAV: $OFFICIAL_WAV；demo_zh 结果保留，L3 标为未测" >&2
    exit 2
  fi
  record_inference "official_llm" "$work_id" "$OFFICIAL_WAV" 240 || exit 1
  append_resource_row "after_official_llm" "$RUN_DIR/rss.csv"

  python3 - "$RUN_DIR" >"$RUN_DIR/summary.txt" <<'PY'
import csv
import os
import sys

run_dir = sys.argv[1]
print("SlotNexus 泰山派语音阶段基线")
print("acoustic_first_sound=not_measured：WAV 模式仅记录 sink 提交/文件完成，未接录音或回环")
print()
print("== 代码版本 ==")
version_path = os.path.join(run_dir, "code_version.txt")
if os.path.exists(version_path):
    with open(version_path, encoding="utf-8") as f:
        for line in f:
            print("  " + line.rstrip())
print()
print("== 六进程启动与 setup ==")
with open(os.path.join(run_dir, "startup.csv"), encoding="utf-8") as f:
    for row in csv.reader(f):
        print("  " + "=".join(row))
print()
print("== 推理阶段 ==")
with open(os.path.join(run_dir, "timings.csv"), encoding="utf-8") as f:
    for row in csv.DictReader(f):
        print("  %(label)s wall=%(wall_ms)sms route=%(route)s tokens=%(token_count)s "
              "pcm=%(pcm_frames)s input_end=%(input_end_ms)s asr_final=%(asr_final_ms)s "
              "llm_first=%(llm_first_token_ms)s first_text=%(first_text_ms)s "
              "tts_first_pcm=%(tts_first_pcm_ms)s first_output=%(first_output_ms)s "
              "output_complete=%(output_complete_ms)s total=%(total_ms)s "
              "wav_complete=%(wav_complete)s playback_complete=%(playback_complete)s" % row)
print()
print("== RSS/温度 ==")
with open(os.path.join(run_dir, "rss.csv"), encoding="utf-8") as f:
    for row in f:
        print("  " + row.rstrip())
PY

  echo "基线原始数据目录: $RUN_DIR"
  echo "代码版本与构建指纹: $RUN_DIR/code_version.txt"
  stop_services
}

# 记录一次固定 WAV 推理的阶段耗时：
# record_inference <标签> <work_id> <wav 路径> <超时秒>
record_inference() {
  local label=$1 work_id=$2 wav=$3 timeout=$4
  local payload t_start t_end raw rc
  payload=$(wav_inference_payload "$work_id" "baseline-$label" "$wav")
  t_start=$(date +%s%N)
  raw=$(probe_gateway "$payload" "$timeout")
  rc=$?
  t_end=$(date +%s%N)
  printf '%s\n' "$raw" >"$RUN_DIR/inference_$label.json"
  if [ "$rc" -ne 0 ]; then
    echo "inference $label 失败（exit=$rc）" >&2
    return 1
  fi
  python3 - "$label" "$raw" "$t_start" "$t_end" "$RUN_DIR/timings.csv" <<'PY'
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

# --------------------------------------------------------------------- wav
# 固定 WAV 全链路回归：demo_zh（修正 ASR + 默认路由 L1）与官方 0.wav
# （BM25 无命中 → L3 真实 RKLLM + MeloTTS）。
scenario_wav() {
  RUN_DIR=/tmp/wav-chain
  rm -rf "$RUN_DIR"
  mkdir -p "$RUN_DIR/session-out" "$RUN_DIR/tts-node"
  if [ ! -f "$OFFICIAL_WAV" ]; then
    echo "缺少官方固定 WAV: $OFFICIAL_WAV（请设置 SLOTNEXUS_ASR_MODEL）" >&2
    exit 1
  fi
  reset_services
  CHAIN_STARTED=1
  start_real_chain
  sleep 2

  echo "== 进程存活 =="
  local p
  for p in "${SERVICES[@]}"; do
    pgrep -x "$p" > /dev/null && echo "$p: alive" || echo "$p: DEAD"
  done

  echo "== setup（会话节点同步 setup 三真实节点，模型加载）=="
  probe_gateway '{"version":1,"type":"setup","request_id":"s-0"}' 60

  echo "== inference 1/2（demo_zh.wav 修正 ASR + 默认路由）=="
  local result
  result=$(probe_gateway "$(wav_inference_payload 'w-0' 'r-wav' 'demo_zh.wav')" 180)
  echo "$result"
  if ! verify_wav_regression "demo_zh.wav" "$result" \
    "你好这是语音合成测试" "l1" "0"; then
    echo "demo_zh.wav 回归失败，终止核验" >&2
    exit 1
  fi

  echo "== inference 2/2（官方 0.wav 固定参考 + 真实 RKLLM/MeloTTS）=="
  local result_llm
  result_llm=$(probe_gateway "$(wav_inference_payload 'w-0' 'r-official' "$OFFICIAL_WAV")" 180)
  echo "$result_llm"
  if ! verify_wav_regression "official 0.wav" "$result_llm" \
    "昨天是 MONDAY TODAY IS THEY AFTER TOMORROW是星期三" "l3" "1"; then
    echo "官方 0.wav 真实 LLM 链路回归失败，终止核验" >&2
    exit 1
  fi

  echo "== taskinfo =="
  probe_gateway '{"version":1,"type":"taskinfo","work_id":"w-0","request_id":"t-1"}' 30

  echo "== 输出文件 =="
  ls -la "$RUN_DIR/session-out" "$RUN_DIR/tts-node" 2>/dev/null | grep -v "^total\|^d"
  if ! check_riff_outputs; then
    echo "固定 WAV 输出 RIFF 校验失败，终止核验" >&2
    exit 1
  fi

  echo "== 真实节点峰值内存 =="
  local pid_name pid
  for pid_name in asr_node llm_node tts_node; do
    pid=$(pgrep -x "$pid_name" | head -1)
    [ -n "$pid" ] && grep -E "VmHWM|VmRSS" "/proc/$pid/status" | sed "s/^/$pid_name: /"
  done

  echo "== 日志尾部 =="
  local f
  for f in "${SERVICES[@]}"; do
    echo "--- $f.log ---"
    tail -6 "$RUN_DIR/$f.log"
  done

  stop_services
}

# --------------------------------------------------------------------- mic
# 现场麦克风闭环：板载 MIC → sherpa 识别 → 路由 → rkllm → melotts 合成输出。
scenario_mic() {
  RUN_DIR=/tmp/mic-chain
  # 录音时长与设备可覆盖：默认 3s 对现场说话偏紧，外接采集设备也要能换。
  RECORD_MS=${SLOTNEXUS_MIC_RECORD_MS:-3000}
  MIC_DEVICE=${SLOTNEXUS_MIC_DEVICE:-plughw:0,0}
  rm -rf "$RUN_DIR"
  mkdir -p "$RUN_DIR/tts-node" "$RUN_DIR/session-out"
  reset_services
  # 板载 MIC 为单端接法（Line Mux=MicL + PGA Mux=Line 2L + 增益拉满），
  # ES8388 默认差分配置采不到信号；录音设备用 plughw:0,0 直通硬件
  # （default 走 PulseAudio，板上时好时坏，录音流会创建失败）。
  amixer -c 0 cset name="Left Line Mux" 3 >/dev/null
  amixer -c 0 cset name="Right Line Mux" 3 >/dev/null
  amixer -c 0 cset name="Left PGA Mux" 1 >/dev/null
  amixer -c 0 cset name="Right PGA Mux" 1 >/dev/null
  amixer -c 0 cset name="Left Channel Capture Volume" 8 >/dev/null
  amixer -c 0 cset name="Right Channel Capture Volume" 8 >/dev/null

  SESSION_EXTRA_ARGS=(--record-device "$MIC_DEVICE" --record-ms "$RECORD_MS")
  CHAIN_STARTED=1
  start_real_chain
  sleep 2

  echo "== setup =="
  probe_gateway '{"version":1,"type":"setup","request_id":"s-0"}' 60

  echo "== 现场麦克风连续采集（VAD 判停，最长 ${RECORD_MS}ms；请对板载麦克风说话）=="
  # setup 刚结束人往往还没准备好，而采集窗口只有几秒：先倒数再发请求，
  # 否则会变成“话没想好就已经录完”，把没说话误当成链路故障。
  local s
  for ((s = 3; s > 0; --s)); do
    printf '\r准备：%ds 后开始录音...' "$s"
    sleep 1
  done
  printf '\r开始录音，请说话！        \n'
  local result mic_check=0
  result=$(probe_gateway '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-mic","payload":{"mode":"stream"}}' 180)
  echo "$result"
  # 采集证据先行：采样数、实际采集时长、RMS/peak 与空读次数，失败时才能
  # 区分“没采到声音”和“采到了但没识别出来”。
  grep -E "session stream" "$RUN_DIR/session_node.log" | tail -2
  verify_mic_regression "$result" || mic_check=$?

  echo "== taskinfo =="
  probe_gateway '{"version":1,"type":"taskinfo","work_id":"w-0","request_id":"t-1"}' 30

  echo "== 输出文件 =="
  ls -la "$RUN_DIR/session-out" "$RUN_DIR/tts-node" 2>/dev/null | grep -v "^total\|^d"
  check_riff_outputs

  echo "== session 日志（输入/麦克风采集/路由/完成）=="
  grep -E "session (req|run|stream|done)" "$RUN_DIR/session_node.log" | tail -8

  stop_services
  if [ "$mic_check" -ne 0 ]; then
    echo "麦克风链路未验证：没有可核验的真人语音 ASR 文本（exit=$mic_check）" >&2
    exit "$mic_check"
  fi
}

verify_mic_regression() {
  python3 - "$1" <<'PY'
import json
import os
import sys

raw = sys.argv[1]
try:
    reply = json.loads(raw)
except json.JSONDecodeError as error:
    print("麦克风响应不是合法 JSON: %s" % error, file=sys.stderr)
    raise SystemExit(2)

payload = reply.get("payload", {})
asr_text = (payload.get("asr_text") or "").strip()
status = payload.get("status")
if not asr_text or status != "ok":
    print("MIC_UNVERIFIED: 无真人语音输入或 ASR 未识别到文本 "
          "(status=%r asr_text=%r)；请对板载麦克风说话后重跑，本次不得计为通过"
          % (status, payload.get("asr_text")))
    raise SystemExit(2)

errors = []
if payload.get("route") != "l2":
    errors.append("route=%r（应为 l2）" % payload.get("route"))
if int(payload.get("token_count") or 0) <= 0:
    errors.append("token_count=%r（应 >0）" % payload.get("token_count"))
if int(payload.get("pcm_frames") or 0) <= 0:
    errors.append("pcm_frames=%r（应 >0）" % payload.get("pcm_frames"))
wav_path = payload.get("wav_path") or ""
if not wav_path or not os.path.isfile(wav_path):
    errors.append("wav_path 不存在: %r" % wav_path)
if errors:
    for item in errors:
        print("麦克风回归失败: " + item, file=sys.stderr)
    raise SystemExit(1)
print("MIC_VERIFIED: asr_text=%r route/llm/tts/RIFF 路径字段齐备" % asr_text)
PY
}

# --------------------------------------------------------------- stability
# 每轮完整重建六进程的重复可用性回归。板端 NPU 驱动持续运行会劣化
# （rkllm 生成中途停滞超时，重启板卡才恢复），进程级重建规避劣化累积；
# 每轮前释放文件缓存（4GB 板内存吃紧）。
scenario_stability() {
  local rounds=${1:-30}
  RUN_DIR=/tmp/stability
  LLM_INFER_TIMEOUT_MS=120000
  rm -rf "$RUN_DIR"
  mkdir -p "$RUN_DIR"
  printf 'round,elapsed_ms,route,token_count,pcm_frames,status,asr_rss_kb,llm_rss_kb,tts_rss_kb,temp_c\n' \
    >"$RUN_DIR/stability.csv"

  local i
  for i in $(seq 1 "$rounds"); do
    run_stability_round "$i"
  done

  echo "== 汇总（$rounds 轮）=="
  python3 - "$RUN_DIR/stability.csv" <<'PY'
import csv
import math
import sys

rows = list(csv.DictReader(open(sys.argv[1])))
n = len(rows)
if n == 0:
   print("无数据")
   sys.exit(0)
times = [int(r["elapsed_ms"]) for r in rows]
ok = [r for r in rows if r["status"] == "ok"]
ts = sorted(times)
p50 = ts[math.ceil(n * 0.50) - 1]
p95 = ts[math.ceil(n * 0.95) - 1]
print("轮次=%d 成功=%d/%d" % (n, len(ok), n))
print("耗时 p50=%dms p95=%dms max=%dms" % (p50, p95, max(times)))
if ok:
   rss0 = [int(ok[0]["asr_rss_kb"]), int(ok[0]["llm_rss_kb"]), int(ok[0]["tts_rss_kb"])]
   rssN = [int(ok[-1]["asr_rss_kb"]), int(ok[-1]["llm_rss_kb"]), int(ok[-1]["tts_rss_kb"])]
   print("RSS 首末轮 asr %d→%dKB / llm %d→%dKB / tts %d→%dKB"
         % (rss0[0], rssN[0], rss0[1], rssN[1], rss0[2], rssN[2]))
   temps = [float(r["temp_c"]) for r in ok if r["temp_c"]]
   if temps:
       print("温度 min=%.1f max=%.1f avg=%.1f C" % (min(temps), max(temps),
                                                    sum(temps) / len(temps)))
print("CSV: %s" % sys.argv[1])
PY
}

run_stability_round() {
  local i=$1
  local base=$RUN_DIR
  local out="$base/r$i"
  mkdir -p "$out/session-out" "$out/tts-node"
  reset_services
  echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null 2>&1
  sleep 1
  CHAIN_STARTED=1
  RUN_DIR="$out"
  start_real_chain
  sleep 2

  probe_gateway '{"version":1,"type":"setup","request_id":"s-0"}' 60 >/dev/null

  local t0 t1 line rc parsed status route token pcm
  t0=$(date +%s%N)
  line=$(probe_gateway "$(wav_inference_payload 'w-0' "r-$i" 'demo_zh.wav')" 200)
  rc=$?
  t1=$(date +%s%N)
  parsed=$(python3 -c '
import json
import sys

try:
    p = json.loads(sys.argv[1]).get("payload", {})
except ValueError:
    print("probe_err|?|?|?")
else:
    print("%s|%s|%s|%s" % (p.get("status"), p.get("route"),
                           p.get("token_count"), p.get("pcm_frames")))
' "$line")
  IFS='|' read -r status route token pcm <<<"$parsed"

  # RSS 与温度要在服务还活着的时候采，停掉再采只会得到 0。
  local asr_rss llm_rss tts_rss temp
  asr_rss=$(read_rss_kb asr_node)
  llm_rss=$(read_rss_kb llm_node)
  tts_rss=$(read_rss_kb tts_node)
  temp=$(read_temp_c)

  local elapsed_ms=$(( (t1 - t0) / 1000000 ))
  stop_services
  RUN_DIR="$base"
  printf '%s,%d,%s,%s,%s,%s,%d,%d,%d,%s\n' "$i" "$elapsed_ms" "$route" \
    "$token" "$pcm" "$status" "$asr_rss" "$llm_rss" "$tts_rss" "$temp" \
    >>"$RUN_DIR/stability.csv"
  local flag="OK "
  [ "$status" = "ok" ] || flag="XX "
  printf '%s轮%s 耗时%5dms route=%s tokens=%s pcm=%s rss=%d/%d/%dKB temp=%s\n' \
    "$flag" "$i" "$elapsed_ms" "$route" "$token" "$pcm" \
    "$asr_rss" "$llm_rss" "$tts_rss" "$temp"
}

# ------------------------------------------------------------------ inject
# 取消 / 节点超时 / 错误输入。llm_node 超时压到 8s，让长思考 prompt 在生成
# 中途触发节点级超时路径。
scenario_inject() {
  RUN_DIR=/tmp/inject-test
  LLM_INFER_TIMEOUT_MS=8000
  rm -rf "$RUN_DIR"
  mkdir -p "$RUN_DIR/tts-node" "$RUN_DIR/session-out"
  reset_services
  CHAIN_STARTED=1
  start_real_chain
  sleep 2

  echo "== 1. 非法 JSON =="
  probe_gateway 'garbage' 10

  echo "== 2. setup =="
  probe_gateway '{"version":1,"type":"setup","request_id":"s-0"}' 60

  echo "== 3. 错误输入：不存在的 WAV =="
  probe_gateway '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-badwav","payload":{"mode":"wav","wav":"no_such.wav"}}' 30

  echo "== 4. 取消：L3 长思考 prompt 推理发起 3s 后 cancel =="
  probe_gateway '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-cancel","payload":{"mode":"text","text":"请详细推导量子力学中薛定谔方程的数学过程，并解释其物理意义和实验验证方法"}}' 120 &
  local probe_pid=$!
  sleep 3
  probe_gateway '{"version":1,"type":"cancel","work_id":"w-0","request_id":"c-1"}' 10
  wait "$probe_pid"

  echo "== 5. 节点推理超时：L3 长思考 prompt 触发 llm_node 8s 超时 =="
  probe_gateway '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-timeout","payload":{"mode":"text","text":"请详细推导量子力学中薛定谔方程的数学过程，并解释其物理意义和实验验证方法"}}' 60

  echo "== 6. taskinfo 收尾（会话仍健康）=="
  probe_gateway '{"version":1,"type":"taskinfo","work_id":"w-0","request_id":"t-1"}' 30

  echo "== 7. 输出残留检查 =="
  ls -la "$RUN_DIR/session-out" "$RUN_DIR/tts-node" 2>/dev/null | grep -v "^total\|^d" \
    || echo "（无输出文件：取消/错误路径未产出音频，符合预期）"

  echo "== 日志尾部 =="
  local f
  for f in session_node llm_node unit_manager edge_gateway; do
    echo "--- $f.log ---"
    tail -4 "$RUN_DIR/$f.log"
  done

  stop_services
}

# --------------------------------------------------------------------- llm
# 仅 llm_node 的固定 prompt 核验（不含 ASR/TTS）。
scenario_llm() {
  RUN_DIR=/tmp/llm-chain
  rm -rf "$RUN_DIR"
  mkdir -p "$RUN_DIR"
  reset_services
  CHAIN_STARTED=1
  start_service llm_node "$BUILD_DIR/apps/llm_node/llm_node" \
    --config "$CONFIG" --infer-timeout-ms "$LLM_INFER_TIMEOUT_MS"
  local llm_pid
  llm_pid=$(cat "$RUN_DIR/llm_node.pid")
  start_service unit_manager "$BUILD_DIR/apps/unit_manager/unit_manager" \
    --module-id voice --default-module voice \
    --node tcp://127.0.0.1:19203 --node-rpc-timeout-ms 30000
  start_service edge_gateway "$BUILD_DIR/apps/edge_gateway/edge_gateway" \
    --forward-timeout-ms 30000
  sleep 2

  echo "== setup =="
  probe_gateway '{"version":1,"type":"setup","request_id":"s-0"}' 30

  echo "== inference（固定 prompt）=="
  probe_gateway '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-llm","payload":{"text":"你好，请用一句话介绍你自己。"}}' 60

  echo "== taskinfo =="
  probe_gateway '{"version":1,"type":"taskinfo","work_id":"w-0","request_id":"t-1"}' 30

  echo "== VmHWM / VmRSS / VmPeak（llm_node 峰值内存）=="
  grep -E "VmHWM|VmRSS|VmPeak" "/proc/$llm_pid/status"

  echo "== llm_node 启动日志 =="
  cat "$RUN_DIR/llm_node.log"

  stop_services
}

# -------------------------------------------------------------------- mock
# Fake Backend 会话链（default 构建产物，无真实模型），用于板上快速自检。
scenario_mock() {
  RUN_DIR=/tmp/slotnexus-session
  BUILD_DIR=${SLOTNEXUS_MOCK_BUILD_DIR:-$DEPLOY_ROOT/build-taishanpi3m}
  rm -rf "$RUN_DIR"
  mkdir -p "$RUN_DIR"
  if [ ! -x "$BUILD_DIR/apps/session_node/session_node" ]; then
    echo "未找到 default 构建产物，请先执行：bash modules/voice/deploy/taishanpi3m/build.sh" >&2
    exit 1
  fi
  if [ ! -f "$MOCK_CONFIG" ]; then
    echo "缺少 mock 场景配置: $MOCK_CONFIG" >&2
    exit 1
  fi
  reset_services
  CHAIN_STARTED=1
  start_service session_node "$BUILD_DIR/apps/session_node/session_node" \
    --listen tcp://127.0.0.1:19210 --config "$MOCK_CONFIG" \
    --output-dir "$RUN_DIR" --fixture-dir "$FIXTURE_DIR"
  start_service unit_manager "$BUILD_DIR/apps/unit_manager/unit_manager" \
    --module-id voice --default-module voice \
    --listen tcp://127.0.0.1:19100 --node tcp://127.0.0.1:19210
  start_service edge_gateway "$BUILD_DIR/apps/edge_gateway/edge_gateway"
  sleep 1
  # 配置里的知识库/夹具缺失都会让 session_node 启动即退出，先查进程再探测，
  # 免得把启动失败报成探测失败。
  if ! kill -0 "$(cat "$RUN_DIR/session_node.pid")" 2>/dev/null; then
    echo "session_node 启动即退出，日志见 $RUN_DIR/session_node.log" >&2
    cat "$RUN_DIR/session_node.log" >&2
    exit 1
  fi

  echo "== 启动日志 =="
  local f
  for f in session_node unit_manager edge_gateway; do
    echo "--- $f.log ---"
    cat "$RUN_DIR/$f.log"
  done

  echo "== 冒烟验证 =="
  mock_probe() {  # mock_probe <名称> <JSON>
    echo "-- $1"
    probe_gateway "$2" 30 | python3 -c "
import sys,json
r=json.load(sys.stdin)
p=r.get('payload',{})
print(' ', r.get('request_id'), '->', 'type='+r['type'], 'status='+str(p.get('status','-')), 'route='+str(p.get('route','-')), 'pcm='+str(p.get('pcm_frames','-')))"
  }
  mock_probe "setup（分配 work_id）" '{"version":1,"type":"setup","request_id":"s-1"}'
  mock_probe "L1 直答（16kHz 音频格式）" '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-l1","payload":{"mode":"text","text":"16kHz 音频格式"}}'
  mock_probe "L3 闲聊（走 Fake LLM）" '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-l3","payload":{"mode":"text","text":"你好 今天天气怎么样"}}'
  mock_probe "固定 WAV 全链路" '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-wav","payload":{"mode":"wav","wav":"voice.wav"}}'
  mock_probe "taskinfo（会话状态）" '{"version":1,"type":"taskinfo","work_id":"w-0","request_id":"t-1"}'
  mock_probe "exit（释放任务）" '{"version":1,"type":"exit","work_id":"w-0","request_id":"e-1"}'

  echo "== WAV 输出 =="
  ls -la "$RUN_DIR"/*.wav 2>/dev/null || echo "（本次未产生 WAV 输出文件）"

  stop_services
}

case "${1:-}" in
  baseline) shift; scenario_baseline "$@" ;;
  wav) shift; scenario_wav "$@" ;;
  mic) shift; scenario_mic "$@" ;;
  stability) shift; scenario_stability "$@" ;;
  inject) shift; scenario_inject "$@" ;;
  llm) shift; scenario_llm "$@" ;;
  mock) shift; scenario_mock "$@" ;;
  *) usage ;;
esac
