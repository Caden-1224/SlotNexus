# 板端运行共享层：路径与依赖根、六进程生命周期、探测与资源采样。
# Author: Caden
#
# 由 start.sh / run.sh / check_deployment.sh source，不直接执行。
# 所有变量都用 ${VAR:-默认} 兜底，因此在 set -euo pipefail 下被 source
# 也安全；部署环境用环境变量覆盖路径，模型与厂商 SDK 不入库。
#
# 这里只放“每个板端场景都要用一遍”的部分：模型根与 LD_LIBRARY_PATH、
# 六个服务的启动参数、优雅退出、gateway 探测、RSS/温度采样、代码指纹。
# 场景差异（节点超时、会话附加参数、探测什么、断言什么）留在调用方，
# 不把差异塞回共享层，免得每个场景都要带一堆开关。

SERVICES=(edge_gateway unit_manager session_node asr_node llm_node tts_node)

# 默认路径与依赖根。源码位置按脚本自身位置推导（板端部署目录不带 .git），
# 外部资产根按环境变量注入。
load_environment() {
  SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
  DEPLOY_ROOT=${SLOTNEXUS_DEPLOY_ROOT:-$(cd "$SCRIPT_DIR/../../../.." && pwd)}
  BUILD_DIR=${SLOTNEXUS_BUILD_DIR:-$DEPLOY_ROOT/build-taishanpi3m-hw}
  CONFIG=${SLOTNEXUS_CONFIG:-$DEPLOY_ROOT/modules/voice/config/taishanpi3m/session.json}
  FIXTURE_DIR=${SLOTNEXUS_VOICE_FIXTURE_DIR:-$DEPLOY_ROOT/data/fixtures}
  RUN_DIR=${SLOTNEXUS_RUN_DIR:-/tmp/slotnexus-runtime}
  ASR_MODEL=${SLOTNEXUS_ASR_MODEL:-$DEPLOY_ROOT/models/sherpa-zipformer-bilingual-zh-en-2023-02-16}
  OFFICIAL_WAV=${OFFICIAL_WAV:-$ASR_MODEL/test_wavs/0.wav}
  # mock 场景用不带 backend 字段的配置，让节点回到内置 Fake 后端；给它
  # 真实后端配置会让 default 构建的 asr_node 直接退出。
  MOCK_CONFIG=${SLOTNEXUS_MOCK_CONFIG:-$DEPLOY_ROOT/modules/voice/config/mock/session.json}
  # 性能测量默认 0：人工阶段等待只用于交互测试，一旦写死就会把测试脚本
  # 自己造出的等待算进模型与链路耗时。
  STAGE_DELAY_MS=${SLOTNEXUS_STAGE_DELAY_MS:-0}
  SINK=${SLOTNEXUS_SINK:-wav}
  SINK_DEVICE=${SLOTNEXUS_SINK_DEVICE:-default}
  ASR_STREAM=${SLOTNEXUS_ASR_STREAM_ENDPOINT:-tcp://127.0.0.1:19205}
  RKLLM_ROOT=${SLOTNEXUS_RKLLM_ROOT:-$HOME/workspace/upstream_rkllm/rknn-llm/rkllm-runtime/Linux/librkllm_api}
  SHERTA_ROOT=${SLOTNEXUS_SHERTA_ROOT:-$HOME/workspace/upstream_rkllm/sherpa-root}
  MELOTTS_ROOT=${SLOTNEXUS_MELOTTS_ROOT:-$HOME/workspace/upstream_melotts}
  export LD_LIBRARY_PATH="$RKLLM_ROOT/aarch64:$SHERTA_ROOT/build/lib:$SHERTA_ROOT/build/_deps/onnxruntime-src/lib:$MELOTTS_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

  # 泰山派 3M（RK3576，4×A72 + 4×A53）默认资源预留：
  #   - LLM 在 session.json 中绑定 CPU4/5/6（3×A72）；
  #   - TTS 钉在 CPU0-3 + CPU7，避免与 LLM 抢 A72，同时保留一颗 A72 给
  #     编码器，板端实测完整链路 wall 明显下降；
  #   - session/控制面钉 CPU0-3，避免占用 A72；
  #   - ASR 不默认钉核，因为它只在 LLM 前运行，放开跑满 A72 反而更快。
  # 环境变量显式设为空串可关闭默认钉核（- 而非 :-，保留空值）。
  SLOTNEXUS_TTS_CPUSET=${SLOTNEXUS_TTS_CPUSET-0-3,7}
  SLOTNEXUS_SESSION_CPUSET=${SLOTNEXUS_SESSION_CPUSET-0-3}
  SLOTNEXUS_CONTROL_CPUSET=${SLOTNEXUS_CONTROL_CPUSET-0-3}
  export SLOTNEXUS_TTS_CPUSET SLOTNEXUS_SESSION_CPUSET SLOTNEXUS_CONTROL_CPUSET
}

# 节点推理超时与会话附加参数：start_real_chain 按场景读取这三项超时和
# SESSION_EXTRA_ARGS（数组），场景只改自己关心的那一项。
ASR_INFER_TIMEOUT_MS=${ASR_INFER_TIMEOUT_MS:-30000}
LLM_INFER_TIMEOUT_MS=${LLM_INFER_TIMEOUT_MS:-60000}
TTS_INFER_TIMEOUT_MS=${TTS_INFER_TIMEOUT_MS:-30000}
SESSION_EXTRA_ARGS=()

# 启动单个后台服务并记录 PID：start_service <名称> <命令...>
# 日志与 PID 文件落在 $RUN_DIR；stop.sh 按 PID 文件优雅退出。
# 可选 CPU 预留：设置 SLOTNEXUS_<服务>_CPUSET 时用 taskset 把该服务
# 整体钉在指定核上。默认不设置，行为与旧版完全一致；板端性能测量可用
# 它把 ASR/TTS/控制面赶到小核，把 A72 留给 RKLLM 内部线程。
start_service() {
  local name=$1
  shift
  local cpuset=""
  case "$name" in
    asr_node) cpuset=${SLOTNEXUS_ASR_CPUSET:-} ;;
    llm_node) cpuset=${SLOTNEXUS_LLM_CPUSET:-} ;;
    tts_node) cpuset=${SLOTNEXUS_TTS_CPUSET:-} ;;
    session_node) cpuset=${SLOTNEXUS_SESSION_CPUSET:-} ;;
    edge_gateway|unit_manager) cpuset=${SLOTNEXUS_CONTROL_CPUSET:-} ;;
  esac
  if [ -n "$cpuset" ]; then
    nohup taskset -c "$cpuset" "$@" >"$RUN_DIR/$name.log" 2>&1 </dev/null &
  else
    nohup "$@" >"$RUN_DIR/$name.log" 2>&1 </dev/null &
  fi
  echo $! >"$RUN_DIR/$name.pid"
}

# 六进程真实链路：三个模型节点 + 会话节点 + 控制面（gateway → manager）。
# 这是全仓库唯一的六进程启动处，端口、端点与超时改这里即可。
start_real_chain() {
  start_service asr_node "$BUILD_DIR/apps/asr_node/asr_node" \
    --listen tcp://127.0.0.1:19201 --config "$CONFIG" \
    --events tcp://127.0.0.1:19421 --events-sync tcp://127.0.0.1:19422 \
    --stream "$ASR_STREAM" \
    --infer-timeout-ms "$ASR_INFER_TIMEOUT_MS"
  start_service llm_node "$BUILD_DIR/apps/llm_node/llm_node" \
    --listen tcp://127.0.0.1:19203 --config "$CONFIG" \
    --events tcp://127.0.0.1:19431 --events-sync tcp://127.0.0.1:19432 \
    --infer-timeout-ms "$LLM_INFER_TIMEOUT_MS"
  start_service tts_node "$BUILD_DIR/apps/tts_node/tts_node" \
    --listen tcp://127.0.0.1:19204 --config "$CONFIG" \
    --output-dir "$RUN_DIR/tts-node" \
    --events tcp://127.0.0.1:19441 --events-sync tcp://127.0.0.1:19442 \
    --infer-timeout-ms "$TTS_INFER_TIMEOUT_MS"
  start_service session_node "$BUILD_DIR/apps/session_node/session_node" \
    --listen tcp://127.0.0.1:19310 --backend net \
    --asr-endpoint tcp://127.0.0.1:19201 \
    --asr-events tcp://127.0.0.1:19421 --asr-events-sync tcp://127.0.0.1:19422 \
    --asr-stream-endpoint "$ASR_STREAM" \
    --llm-endpoint tcp://127.0.0.1:19203 \
    --llm-events tcp://127.0.0.1:19431 --llm-events-sync tcp://127.0.0.1:19432 \
    --tts-endpoint tcp://127.0.0.1:19204 \
    --tts-events tcp://127.0.0.1:19441 --tts-events-sync tcp://127.0.0.1:19442 \
    --net-setup-timeout-ms 60000 --net-rpc-timeout-ms 120000 \
    --config "$CONFIG" --output-dir "$RUN_DIR/session-out" \
    --fixture-dir "$FIXTURE_DIR" --sink "$SINK" \
    --sink-device "$SINK_DEVICE" --stage-delay-ms "$STAGE_DELAY_MS" \
    "${SESSION_EXTRA_ARGS[@]}"
  start_service unit_manager "$BUILD_DIR/apps/unit_manager/unit_manager" \
    --module-id voice --default-module voice \
    --node tcp://127.0.0.1:19310 --node-rpc-timeout-ms 120000
  start_service edge_gateway "$BUILD_DIR/apps/edge_gateway/edge_gateway" \
    --forward-timeout-ms 120000
}

# 清场：强杀可能残留的六进程并删除 PID 文件（幂等）。
reset_services() {
  SLOTNEXUS_RUN_DIR="$RUN_DIR" bash "$SCRIPT_DIR/stop.sh" --force >/dev/null
}

# 停止六进程：TERM 优雅退出 → 轮询等待 → KILL → 清理 PID 文件。
# 复用 stop.sh，避免每个场景各写一遍退出轮询与耗时统计。
stop_services() {
  SLOTNEXUS_RUN_DIR="$RUN_DIR" bash "$SCRIPT_DIR/stop.sh"
}

# 经 edge_gateway(9100) 探测会话：probe_gateway <请求 JSON> <超时秒>
probe_gateway() {
  python3 "$DEPLOY_ROOT/scripts/gateway_probe.py" 9100 "$1" "$2"
}

# 单进程 RSS(kB)：多实例累加，进程不存在时为 0。
read_rss_kb() {
  local name=$1 total=0 pid kb
  for pid in $(pgrep -x "$name" 2>/dev/null); do
    kb=$(awk '/^VmRSS:/ { print $2 }' "/proc/$pid/status" 2>/dev/null)
    [ -n "$kb" ] && total=$((total + kb))
  done
  printf '%s\n' "$total"
}

# SoC 温度(℃)：不同 BSP 的 thermal zone 数量不同，读不到时留空而不是报错。
read_temp_c() {
  local zone
  for zone in /sys/class/thermal/thermal_zone0 /sys/class/thermal/thermal_zone1; do
    if [ -r "$zone/temp" ]; then
      awk '{ printf "%.1f", $1 / 1000; exit }' "$zone/temp" 2>/dev/null && return 0
    fi
  done
  printf '\n'
}

# 追加一行 RSS/温度采样：append_resource_row <标签> <csv 路径>
append_resource_row() {
  local label=$1 csv=$2 row="$1" name
  for name in asr_node llm_node tts_node session_node edge_gateway unit_manager; do
    row="$row,$(read_rss_kb "$name")"
  done
  row="$row,$(read_temp_c)"
  if [ ! -f "$csv" ]; then
    printf 'label,asr_node_rss_kb,llm_node_rss_kb,tts_node_rss_kb,session_node_rss_kb,edge_gateway_rss_kb,unit_manager_rss_kb,temp_c\n' >>"$csv"
  fi
  printf '%s\n' "$row" >>"$csv"
}

# 代码版本指纹：板端部署目录通常不带 .git，因此同时记录 Git commit
# （可用时）、源码内容哈希、六个可执行文件与配置哈希，保证“这组数字出自
# 哪份代码”可核对，而不是只写一个版本号。
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
    local name
    for name in "${SERVICES[@]}"; do
      if [ -x "$BUILD_DIR/apps/$name/$name" ]; then
        echo "binary_${name}_sha256=$(sha256sum "$BUILD_DIR/apps/$name/$name" | cut -d' ' -f1)"
      else
        echo "binary_${name}_sha256=missing"
      fi
    done
    echo "config_sha256=$(sha256sum "$CONFIG" 2>/dev/null | cut -d' ' -f1)"
    echo "stage_delay_ms=$STAGE_DELAY_MS"
    echo "captured_at=$(date -Is)"
  } >"$RUN_DIR/code_version.txt"
}

# 校验 $RUN_DIR/session-out 与 tts-node 下的输出都是 RIFF/WAVE，且至少一个。
check_riff_outputs() {
  python3 - "$RUN_DIR" <<'PY'
import glob
import os
import struct
import sys

run_dir = sys.argv[1]
wavs = sorted(glob.glob(os.path.join(run_dir, "session-out", "*.wav"))) + \
       sorted(glob.glob(os.path.join(run_dir, "tts-node", "*.wav")))
if not wavs:
    print("未发现任何输出 WAV", file=sys.stderr)
    raise SystemExit(1)
for path in wavs:
    with open(path, "rb") as fh:
        header = fh.read(12)
    if len(header) != 12 or header[:4] != b"RIFF" or header[8:12] != b"WAVE":
        print("输出不是 RIFF/WAVE: %s" % path, file=sys.stderr)
        raise SystemExit(1)
    print("  RIFF OK 块大小=%d %s"
          % (struct.unpack('<I', header[4:8])[0], os.path.basename(path)))
print("输出 RIFF/WAVE 校验通过: %d 个文件" % len(wavs))
PY
}

# 固定 WAV 回归校验：verify_wav_regression <标签> <响应 JSON> <期望 ASR 文本>
#                                   <期望路由> <是否需要 LLM(0/1)>
verify_wav_regression() {
  python3 - "$@" <<'PY'
import json
import os
import sys

label, raw, expected_asr, expected_route, require_llm = sys.argv[1:]
try:
    reply = json.loads(raw)
except json.JSONDecodeError as error:
    print("%s 响应不是合法 JSON: %s" % (label, error), file=sys.stderr)
    raise SystemExit(1)

payload = reply.get("payload", {})
status = payload.get("status")
asr_text = (payload.get("asr_text") or "").replace(" ", "")
expected_asr = expected_asr.replace(" ", "")
route = payload.get("route")
token_count = int(payload.get("token_count") or 0)
pcm_frames = int(payload.get("pcm_frames") or 0)
final_text = payload.get("final_text") or ""
wav_path = payload.get("wav_path") or ""

errors = []
if status != "ok":
    errors.append("status=%r（应为 ok）" % status)
if asr_text != expected_asr:
    errors.append("asr_text=%r（应为 %s）" % (payload.get("asr_text"), expected_asr))
if route != expected_route:
    errors.append("route=%r（应为 %s）" % (route, expected_route))
if require_llm == "1" and token_count <= 0:
    errors.append("token_count=%r（L2/L3 应 >0）" % token_count)
if pcm_frames <= 0:
    errors.append("pcm_frames=%r（应 >0）" % pcm_frames)
if not final_text.strip():
    errors.append("final_text 为空")
if not wav_path or not os.path.isfile(wav_path):
    errors.append("wav_path 不存在: %r" % wav_path)
if errors:
    for item in errors:
        print("%s 回归失败: %s" % (label, item), file=sys.stderr)
    raise SystemExit(1)
print("%s 回归通过: asr=%r route=%s tokens=%d pcm=%d"
      % (label, payload.get("asr_text"), route, token_count, pcm_frames))
PY
}
