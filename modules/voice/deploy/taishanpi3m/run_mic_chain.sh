#!/bin/bash
# Author: Caden
# 板端核验：现场麦克风闭环——语音 → ALSA 录音（ES8323 板载
# 麦克风）→ sherpa 识别 → 路由 → rkllm 回答 → melotts 合成输出。
# 输入负载 {"mode":"alsa"}：session_node 按 --record-ms 阻塞采集录音，
# 样本随管线 kMic 输入（音频上行模式 → asr 节点 pcm64 上行）。
# 录音通路：板载 MIC 为单端接法（Line Mux=MicL + PGA Mux=Line 2L + 增益
# 拉满），ES8388 默认差分配置采不到信号；录音设备用 plughw:0,0 直通
# 硬件（default 走 PulseAudio，板上时好时坏，录音流会创建失败）。
# 依赖根可用环境变量覆盖（与 run_real_wav_chain.sh 一致）。
# 用法：板端执行（录音期间请对板载麦克风说话）。输出 /tmp/mic-chain/。
set -u
FIXTURE_DIR=${SLOTNEXUS_VOICE_FIXTURE_DIR:-$PWD/data/fixtures}
DEPLOY_ROOT=${SLOTNEXUS_DEPLOY_ROOT:-$HOME/workspace/slotnexus-runtime}
RKLLM_ROOT=${SLOTNEXUS_RKLLM_ROOT:-$HOME/workspace/upstream_rkllm/rknn-llm/rkllm-runtime/Linux/librkllm_api}
SHERTA_ROOT=${SLOTNEXUS_SHERTA_ROOT:-$HOME/workspace/upstream_rkllm/sherpa-root}
MELOTTS_ROOT=${SLOTNEXUS_MELOTTS_ROOT:-$HOME/workspace/upstream_melotts}
cd "$DEPLOY_ROOT" || exit 1
export LD_LIBRARY_PATH="$RKLLM_ROOT/aarch64:$SHERTA_ROOT/build/lib:$SHERTA_ROOT/build/_deps/onnxruntime-src/lib:$MELOTTS_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
OUT=/tmp/mic-chain
rm -rf "$OUT"
mkdir -p "$OUT/tts-node" "$OUT/session-out"
for p in edge_gateway unit_manager session_node asr_node llm_node tts_node; do pkill -TERM -x "$p" 2>/dev/null; done
sleep 0.5
# ES8388 录音通路（板载 MIC 单端 → MicL → PGA Line 2L，增益 8/8 拉满）。
amixer -c 0 cset name="Left Line Mux" 3 >/dev/null
amixer -c 0 cset name="Right Line Mux" 3 >/dev/null
amixer -c 0 cset name="Left PGA Mux" 1 >/dev/null
amixer -c 0 cset name="Right PGA Mux" 1 >/dev/null
amixer -c 0 cset name="Left Channel Capture Volume" 8 >/dev/null
amixer -c 0 cset name="Right Channel Capture Volume" 8 >/dev/null

./build-taishanpi3m-hw/apps/asr_node/asr_node \
 --listen tcp://127.0.0.1:19201 --config modules/voice/config/taishanpi3m/session.json \
 --events tcp://127.0.0.1:19421 --events-sync tcp://127.0.0.1:19422 \
 --infer-timeout-ms 30000 > "$OUT/asr.log" 2>&1 &
./build-taishanpi3m-hw/apps/llm_node/llm_node \
 --listen tcp://127.0.0.1:19203 --config modules/voice/config/taishanpi3m/session.json \
 --events tcp://127.0.0.1:19431 --events-sync tcp://127.0.0.1:19432 \
 --infer-timeout-ms 60000 > "$OUT/llm.log" 2>&1 &
./build-taishanpi3m-hw/apps/tts_node/tts_node \
 --listen tcp://127.0.0.1:19204 --config modules/voice/config/taishanpi3m/session.json \
 --output-dir "$OUT/tts-node" \
 --events tcp://127.0.0.1:19441 --events-sync tcp://127.0.0.1:19442 \
 --infer-timeout-ms 30000 > "$OUT/tts.log" 2>&1 &
./build-taishanpi3m-hw/apps/session_node/session_node \
 --listen tcp://127.0.0.1:19310 --backend net --asr-uplink \
 --asr-endpoint tcp://127.0.0.1:19201 --asr-events tcp://127.0.0.1:19421 --asr-events-sync tcp://127.0.0.1:19422 \
 --llm-endpoint tcp://127.0.0.1:19203 --llm-events tcp://127.0.0.1:19431 --llm-events-sync tcp://127.0.0.1:19432 \
 --tts-endpoint tcp://127.0.0.1:19204 --tts-events tcp://127.0.0.1:19441 --tts-events-sync tcp://127.0.0.1:19442 \
 --net-setup-timeout-ms 60000 --net-rpc-timeout-ms 60000 \
 --record-device plughw:0,0 --record-ms 3000 \
 --config modules/voice/config/taishanpi3m/session.json --output-dir "$OUT/session-out" \
 --fixture-dir "$FIXTURE_DIR" --stage-delay-ms 20 > "$OUT/session.log" 2>&1 &
./build-taishanpi3m-hw/apps/unit_manager/unit_manager --module-id voice --default-module voice \
 --node tcp://127.0.0.1:19310 --node-rpc-timeout-ms 120000 > "$OUT/manager.log" 2>&1 &
./build-taishanpi3m-hw/apps/edge_gateway/edge_gateway \
 --forward-timeout-ms 120000 > "$OUT/gateway.log" 2>&1 &
sleep 2

echo "== setup =="
python3 scripts/gateway_probe.py 9100 '{"version":1,"type":"setup","request_id":"s-0"}' 60

echo "== 现场麦克风推理（3s 录音，请对板载麦克风说话）=="
RESULT=$(python3 scripts/gateway_probe.py 9100 '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-mic","payload":{"mode":"alsa"}}' 180)
echo "$RESULT"
python3 - "$RESULT" <<'PY'
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
MIC_CHECK=$?

echo "== taskinfo =="
python3 scripts/gateway_probe.py 9100 '{"version":1,"type":"taskinfo","work_id":"w-0","request_id":"t-1"}' 30

echo "== 输出文件 =="
ls -la "$OUT/session-out" "$OUT/tts-node" 2>/dev/null | grep -v "^total\|^d"
for f in "$OUT/session-out"/*.wav; do
 [ -f "$f" ] && python3 - "$f" <<'EOF'
import struct, sys
d = open(sys.argv[1], "rb").read(12)
if len(d) == 12 and d[:4] == b"RIFF" and d[8:12] == b"WAVE":
   print("  RIFF OK 块大小=%d" % struct.unpack('<I', d[4:8])[0])
else:
   print("  非 RIFF！")
EOF
done

echo "== session 日志（输入/麦克风采集/路由/完成）=="
grep -E "session (req|run|mic|done)" "$OUT/session.log" | tail -8

echo "== 优雅退出 =="
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
 echo "仍有进程存活" >&2
 exit 1
fi
echo "全部进程已退出"
if [ "$MIC_CHECK" -ne 0 ]; then
 echo "麦克风链路未验证：没有可核验的真人语音 ASR 文本（exit=$MIC_CHECK）" >&2
 exit "$MIC_CHECK"
fi
exit 0
