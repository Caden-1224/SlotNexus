#!/bin/bash
# 板端核验：现场麦克风闭环——语音 → ALSA 录音（ES8323 板载
# 麦克风）→ sherpa 识别 → 路由 → rkllm 回答 → summertts 合成输出。
# 输入负载 {"mode":"alsa"}：session_node 按 --record-ms 阻塞采集录音，
# 样本随管线 kMic 输入（音频上行模式 → asr 节点 pcm64 上行）。
# 录音通路：板载 MIC 为单端接法（Line Mux=MicL + PGA Mux=Line 2L + 增益
# 拉满），ES8388 默认差分配置采不到信号；录音设备用 plughw:0,0 直通
# 硬件（default 走 PulseAudio，板上时好时坏，录音流会创建失败）。
# 用法：板端执行（录音期间请对板载麦克风说话）。输出 /tmp/mic-chain/。
set -u
cd ~/workspace/slotnexus-runtime
export LD_LIBRARY_PATH=/home/lckfb/workspace/upstream_rkllm/rknn-llm/rkllm-runtime/Linux/librkllm_api/aarch64
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
 --listen tcp://127.0.0.1:19201 --config config/taishanpi3m/session.json \
 --events tcp://127.0.0.1:19421 --events-sync tcp://127.0.0.1:19422 \
 --infer-timeout-ms 30000 > "$OUT/asr.log" 2>&1 &
./build-taishanpi3m-hw/apps/llm_node/llm_node \
 --listen tcp://127.0.0.1:19203 --config config/taishanpi3m/session.json \
 --events tcp://127.0.0.1:19431 --events-sync tcp://127.0.0.1:19432 \
 --infer-timeout-ms 60000 > "$OUT/llm.log" 2>&1 &
./build-taishanpi3m-hw/apps/tts_node/tts_node \
 --listen tcp://127.0.0.1:19204 --config config/taishanpi3m/session.json \
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
 --config config/taishanpi3m/session.json --output-dir "$OUT/session-out" \
 --fixture-dir data/fixtures --stage-delay-ms 20 > "$OUT/session.log" 2>&1 &
./build-taishanpi3m-hw/apps/unit_manager/unit_manager \
 --node tcp://127.0.0.1:19310 --node-rpc-timeout-ms 120000 > "$OUT/manager.log" 2>&1 &
./build-taishanpi3m-hw/apps/edge_gateway/edge_gateway \
 --forward-timeout-ms 120000 > "$OUT/gateway.log" 2>&1 &
sleep 2

echo "== setup =="
python3 scripts/gateway_probe.py 9100 '{"version":1,"type":"setup","request_id":"s-0"}' 60

echo "== 现场麦克风推理（3s 录音，请对板载麦克风说话）=="
python3 scripts/gateway_probe.py 9100 '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-mic","payload":{"mode":"alsa"}}' 180

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

echo "== session 日志（输入/路由/完成）=="
grep -E "session (req|run|done)" "$OUT/session.log" | tail -5

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
[ "$ALIVE" = 1 ] && echo "仍有进程存活" || echo "全部进程已退出"
