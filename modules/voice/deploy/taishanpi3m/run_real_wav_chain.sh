#!/bin/bash
# Author: Caden
# 板端核验：真机固定 WAV 全链路基线。
# gateway → manager → session_node（--backend net --asr-uplink）→ 三真实节点：
#   asr_node（sherpa_onnx 识别）/ llm_node（rkllm 推理）/ tts_node（melotts 合成）。
# 负载 1：data/fixtures/demo_zh.wav（固定中文语音，16 kHz/16-bit/单声道，
#   参考文本「你好这是语音合成测试」；修正 ASR 后默认路由为 L1 事实直答）。
# 负载 2：模型自带 test_wavs/0.wav（sherpa-onnx 官方固定测试音频，
#   参考文本见官方模型页：昨天是 MONDAY TODAY IS THEY AFTER TOMORROW是星期三；
#   BM25 无命中路由 L3，真实走 RKLLM + MeloTTS）。
# 两个负载都经会话侧 PCM 累积上行 asr 节点，覆盖固定 WAV 全链路与真实 LLM。
# 输出 /tmp/wav-chain/，逐项核验后优雅退出。
#
# 依赖根可用环境变量覆盖（缺省与本机板端目录一致）：
#   SLOTNEXUS_DEPLOY_ROOT / SLOTNEXUS_RKLLM_ROOT / SLOTNEXUS_SHERTA_ROOT /
#   SLOTNEXUS_MELOTTS_ROOT / SLOTNEXUS_ASR_MODEL / SLOTNEXUS_VOICE_FIXTURE_DIR
# 用法：板端执行。
set -u
FIXTURE_DIR=${SLOTNEXUS_VOICE_FIXTURE_DIR:-$PWD/data/fixtures}
DEPLOY_ROOT=${SLOTNEXUS_DEPLOY_ROOT:-$HOME/workspace/slotnexus-runtime}
RKLLM_ROOT=${SLOTNEXUS_RKLLM_ROOT:-$HOME/workspace/upstream_rkllm/rknn-llm/rkllm-runtime/Linux/librkllm_api}
SHERTA_ROOT=${SLOTNEXUS_SHERTA_ROOT:-$HOME/workspace/upstream_rkllm/sherpa-root}
MELOTTS_ROOT=${SLOTNEXUS_MELOTTS_ROOT:-$HOME/workspace/upstream_melotts}
# 性能测量默认不注入人工阶段等待；需要模拟慢消费时显式设置环境变量。
STAGE_DELAY_MS=${SLOTNEXUS_STAGE_DELAY_MS:-0}
ASR_MODEL=${SLOTNEXUS_ASR_MODEL:-$DEPLOY_ROOT/models/sherpa-zipformer-bilingual-zh-en-2023-02-16}
OFFICIAL_WAV=${OFFICIAL_WAV:-$ASR_MODEL/test_wavs/0.wav}
cd "$DEPLOY_ROOT" || exit 1
export LD_LIBRARY_PATH="$RKLLM_ROOT/aarch64:$SHERTA_ROOT/build/lib:$SHERTA_ROOT/build/_deps/onnxruntime-src/lib:$MELOTTS_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
OUT=/tmp/wav-chain
rm -rf "$OUT"
mkdir -p "$OUT/session-out" "$OUT/tts-node"
if [ ! -f "$OFFICIAL_WAV" ]; then
 echo "缺少官方固定 WAV: $OFFICIAL_WAV（请设置 SLOTNEXUS_ASR_MODEL）" >&2
 exit 1
fi
for p in edge_gateway unit_manager session_node asr_node llm_node tts_node; do pkill -TERM -x "$p" 2>/dev/null; done
sleep 0.5

# 三真实节点（控制面 RPC + 数据面事件 PUB 出口）。
./build-taishanpi3m-hw/apps/asr_node/asr_node \
 --listen tcp://127.0.0.1:19201 --config modules/voice/config/taishanpi3m/session.json \
 --events tcp://127.0.0.1:19421 --events-sync tcp://127.0.0.1:19422 \
 --infer-timeout-ms 30000 > "$OUT/asr.log" 2>&1 &
ASRPID=$!
./build-taishanpi3m-hw/apps/llm_node/llm_node \
 --listen tcp://127.0.0.1:19203 --config modules/voice/config/taishanpi3m/session.json \
 --events tcp://127.0.0.1:19431 --events-sync tcp://127.0.0.1:19432 \
 --infer-timeout-ms 60000 > "$OUT/llm.log" 2>&1 &
LLMPID=$!
./build-taishanpi3m-hw/apps/tts_node/tts_node \
 --listen tcp://127.0.0.1:19204 --config modules/voice/config/taishanpi3m/session.json \
 --output-dir "$OUT/tts-node" \
 --events tcp://127.0.0.1:19441 --events-sync tcp://127.0.0.1:19442 \
 --infer-timeout-ms 30000 > "$OUT/tts.log" 2>&1 &
TTSPID=$!
# 会话节点：网络后端（控制面 RPC 上行 + 数据面事件回放），音频上行真实负载。
./build-taishanpi3m-hw/apps/session_node/session_node \
 --listen tcp://127.0.0.1:19310 --backend net --asr-uplink \
 --asr-endpoint tcp://127.0.0.1:19201 --asr-events tcp://127.0.0.1:19421 --asr-events-sync tcp://127.0.0.1:19422 \
 --llm-endpoint tcp://127.0.0.1:19203 --llm-events tcp://127.0.0.1:19431 --llm-events-sync tcp://127.0.0.1:19432 \
 --tts-endpoint tcp://127.0.0.1:19204 --tts-events tcp://127.0.0.1:19441 --tts-events-sync tcp://127.0.0.1:19442 \
 --net-setup-timeout-ms 60000 --net-rpc-timeout-ms 60000 \
 --config modules/voice/config/taishanpi3m/session.json --output-dir "$OUT/session-out" \
 --fixture-dir "$FIXTURE_DIR" --stage-delay-ms "$STAGE_DELAY_MS" > "$OUT/session.log" 2>&1 &
SESSID=$!
# 控制面：gateway → manager（轮转单节点）→ session_node。
./build-taishanpi3m-hw/apps/unit_manager/unit_manager --module-id voice --default-module voice \
 --node tcp://127.0.0.1:19310 --node-rpc-timeout-ms 120000 > "$OUT/manager.log" 2>&1 &
./build-taishanpi3m-hw/apps/edge_gateway/edge_gateway \
 --forward-timeout-ms 120000 > "$OUT/gateway.log" 2>&1 &
sleep 2

echo "== 进程存活 =="
for p in edge_gateway unit_manager session_node asr_node llm_node tts_node; do
 pgrep -x "$p" > /dev/null && echo "$p: alive" || echo "$p: DEAD"
done

echo "== setup（会话节点同步 setup 三真实节点，模型加载）=="
python3 scripts/gateway_probe.py 9100 '{"version":1,"type":"setup","request_id":"s-0"}' 60

echo "== inference 1/2（demo_zh.wav 修正 ASR + 默认路由）=="
RESULT=$(python3 scripts/gateway_probe.py 9100 '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-wav","payload":{"mode":"wav","wav":"demo_zh.wav"}}' 180)
echo "$RESULT"
python3 - "demo_zh.wav" "$RESULT" "你好这是语音合成测试" "l1" "0" <<'PY'
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
print("%s 回归通过: asr=%r route=%s tokens=%d pcm=%d" %
      (label, payload.get("asr_text"), route, token_count, pcm_frames))
PY
if [ $? -ne 0 ]; then
 echo "demo_zh.wav 回归失败，终止核验" >&2
 exit 1
fi

echo "== inference 2/2（官方 0.wav 固定参考 + 真实 RKLLM/MeloTTS）=="
RESULT_LLM=$(python3 scripts/gateway_probe.py 9100 "{\"version\":1,\"type\":\"inference\",\"work_id\":\"w-0\",\"request_id\":\"r-official\",\"payload\":{\"mode\":\"wav\",\"wav\":\"$OFFICIAL_WAV\"}}" 180)
echo "$RESULT_LLM"
python3 - "official 0.wav" "$RESULT_LLM" "昨天是 MONDAY TODAY IS THEY AFTER TOMORROW是星期三" "l3" "1" <<'PY'
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
print("%s 回归通过: asr=%r route=%s tokens=%d pcm=%d" %
      (label, payload.get("asr_text"), route, token_count, pcm_frames))
PY
if [ $? -ne 0 ]; then
 echo "官方 0.wav 真实 LLM 链路回归失败，终止核验" >&2
 exit 1
fi

echo "== taskinfo =="
python3 scripts/gateway_probe.py 9100 '{"version":1,"type":"taskinfo","work_id":"w-0","request_id":"t-1"}' 30

echo "== 输出文件 =="
ls -la "$OUT/session-out" "$OUT/tts-node" 2>/dev/null
for f in "$OUT/session-out"/*.wav "$OUT/tts-node"/*.wav; do
 [ -f "$f" ] && python3 - "$f" <<'EOF'
import struct, sys
p = sys.argv[1]
with open(p, "rb") as fh:
   d = fh.read(12)
   if len(d) == 12 and d[:4] == b"RIFF" and d[8:12] == b"WAVE":
       print("  RIFF OK 块大小=%d" % struct.unpack('<I', d[4:8])[0])
   else:
       print("  非 RIFF！")
EOF
done
python3 - "$OUT" <<'PY'
import glob
import os
import struct
import sys

out = sys.argv[1]
wavs = sorted(glob.glob(os.path.join(out, "session-out", "*.wav"))) + \
       sorted(glob.glob(os.path.join(out, "tts-node", "*.wav")))
if not wavs:
    print("未发现任何输出 WAV", file=sys.stderr)
    raise SystemExit(1)
for path in wavs:
    with open(path, "rb") as fh:
        header = fh.read(12)
    if len(header) != 12 or header[:4] != b"RIFF" or header[8:12] != b"WAVE":
        print("输出不是 RIFF/WAVE: %s" % path, file=sys.stderr)
        raise SystemExit(1)
print("输出 RIFF/WAVE 校验通过: %d 个文件" % len(wavs))
PY
if [ $? -ne 0 ]; then
 echo "固定 WAV 输出 RIFF 校验失败，终止核验" >&2
 exit 1
fi

echo "== 真实节点峰值内存 =="
for pid_name in asr_node llm_node tts_node; do
 PID=$(pgrep -x "$pid_name" | head -1)
 [ -n "$PID" ] && grep -E "VmHWM|VmRSS" /proc/$PID/status | sed "s/^/$pid_name: /"
done

echo "== 日志尾部 =="
for f in asr llm tts session manager gateway; do echo "--- $f.log ---"; tail -6 "$OUT/$f.log"; done

echo "== 优雅退出（轮询等待，记录最长退出耗时）=="
for p in edge_gateway unit_manager session_node asr_node llm_node tts_node; do pkill -TERM -x "$p" 2>/dev/null; done
T0=$(date +%s%N)
ALIVE=1
for i in $(seq 1 40); do
 ALIVE=0
 for p in edge_gateway unit_manager session_node asr_node llm_node tts_node; do
   pgrep -x "$p" > /dev/null && ALIVE=1
 done
 [ "$ALIVE" = 0 ] && break
 sleep 0.5
done
T1=$(date +%s%N)
echo "退出耗时: $(( (T1 - T0) / 1000000000 ))s（20s 上限）"
if [ "$ALIVE" = 1 ]; then
 for p in edge_gateway unit_manager session_node asr_node llm_node tts_node; do
   pgrep -x "$p" > /dev/null && echo "$p 仍存活（20s 后）"
 done
else
 echo "全部进程已退出"
fi
