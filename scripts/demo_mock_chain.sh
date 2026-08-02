#!/bin/bash
# Mock 全链路演示：五节点（echo/asr/rag/llm/tts）+ Manager + 网关。
#
# 展示真实运行效果：进程启动、work_id 轮转路由、逐节点推理输出、
# TTS 产出的 WAV 文件、SIGTERM 优雅退出。日志落在 /tmp/voxorchestra-demo/。
#
# 用法：scripts/demo_mock_chain.sh
set -u
cd "$(dirname "$0")/.."
B=build-wsl
OUT=/tmp/voxorchestra-demo
mkdir -p "$OUT"

echo "== 清理残留进程 =="
for p in echo_node asr_node rag_node llm_node tts_node unit_manager edge_gateway; do
  pkill -TERM -x "$p" 2>/dev/null
done
sleep 0.3
rm -f "$OUT"/*.log "$OUT"/*.wav

echo "== 启动五节点 + Manager + 网关（日志见 $OUT/）=="
"$B/apps/echo_node/echo_node" > "$OUT/echo.log" 2>&1 &
"$B/apps/asr_node/asr_node" > "$OUT/asr.log" 2>&1 &
"$B/apps/rag_node/rag_node" > "$OUT/rag.log" 2>&1 &
"$B/apps/llm_node/llm_node" > "$OUT/llm.log" 2>&1 &
"$B/apps/tts_node/tts_node" --output-dir "$OUT" > "$OUT/tts.log" 2>&1 &
"$B/apps/unit_manager/unit_manager" \
  --node tcp://127.0.0.1:19200 --node tcp://127.0.0.1:19201 \
  --node tcp://127.0.0.1:19202 --node tcp://127.0.0.1:19203 \
  --node tcp://127.0.0.1:19204 > "$OUT/manager.log" 2>&1 &
"$B/apps/edge_gateway/edge_gateway" > "$OUT/gateway.log" 2>&1 &
sleep 0.6

echo "== 各进程启动日志 =="
for f in echo asr rag llm tts manager gateway; do
  echo "--- $f.log ---"
  cat "$OUT/$f.log"
done

echo "== setup ×5：Manager 轮转分配全局 work_id =="
for i in 0 1 2 3 4; do
  python3 scripts/gateway_probe.py 9100 \
    "{\"version\":1,\"type\":\"setup\",\"request_id\":\"s-$i\"}" \
    | python3 -c "import sys,json; r=json.load(sys.stdin); print('setup', r['request_id'], '->', r['work_id'])"
done

echo "== 每个节点各发一次推理（注意 request_id 一一对应）=="
echo "-- echo（19200）"
python3 scripts/gateway_probe.py 9100 '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-echo","payload":{"text":"你好"}}' \
  | python3 -c "import sys,json; r=json.load(sys.stdin); print(r['request_id'], '->', r['payload'].get('text','(error)'))"
echo "-- asr（19201）：3 帧音频 -> 逐帧 partial 汇总"
python3 scripts/gateway_probe.py 9100 '{"version":1,"type":"inference","work_id":"w-1","request_id":"r-asr","payload":{"text":"3"}}' \
  | python3 -c "import sys,json; r=json.load(sys.stdin); print(r['request_id'], '->', r['payload'].get('text','(error)'))"
echo "-- rag（19202）：查询 -> L0-L3 路由证据（级别/Top-2 知识块/直答或提示词）"
python3 scripts/gateway_probe.py 9100 '{"version":1,"type":"inference","work_id":"w-2","request_id":"r-rag","payload":{"text":"多进程架构怎么实现"}}' \
  | python3 -c "import sys,json; r=json.load(sys.stdin); print(r['request_id'], '->', r['payload'].get('text','(error)'))"
echo "-- llm（19203）：prompt -> 逐词 token 回显"
python3 scripts/gateway_probe.py 9100 '{"version":1,"type":"inference","work_id":"w-3","request_id":"r-llm","payload":{"text":"你好 世界 VoxOrchestra"}}' \
  | python3 -c "import sys,json; r=json.load(sys.stdin); print(r['request_id'], '->', r['payload'].get('text','(error)'))"
echo "-- tts（19204）：长文本 -> 约 1 秒可听 WAV"
TTS_PAYLOAD=$(python3 -c "import json; print(json.dumps({'version':1,'type':'inference','work_id':'w-4','request_id':'r-tts','payload':{'text':'滴'*600}}))")
python3 scripts/gateway_probe.py 9100 "$TTS_PAYLOAD" \
  | python3 -c "import sys,json; r=json.load(sys.stdin); print(r['request_id'], '->', r['payload'].get('text','(error)'))"

echo "== TTS 产出文件（可拷到 Windows 播放）=="
ls -la "$OUT"/*.wav
python3 -c "import wave,glob; w=wave.open(glob.glob('$OUT/*.wav')[0],'rb'); print('WAV: 采样率 %d Hz, 单声道, %d 帧 (%.2f 秒)' % (w.getframerate(), w.getnframes(), w.getnframes()/w.getframerate()))"

echo "== taskinfo：w-1（asr）已推理 1 次，状态 ready =="
python3 scripts/gateway_probe.py 9100 '{"version":1,"type":"taskinfo","work_id":"w-1","request_id":"t-1"}' \
  | python3 -c "import sys,json; r=json.load(sys.stdin); p=r['payload']; print('state=%s inference_count=%s in_flight=%s' % (p['state'], p['inference_count'], p['in_flight']))"

echo "== SIGTERM 优雅退出 =="
for p in edge_gateway unit_manager echo_node asr_node rag_node llm_node tts_node; do
  pkill -TERM -x "$p"
done
sleep 0.6
if pgrep -x edge_gateway > /dev/null; then echo "仍有进程存活！"; else echo "全部进程已退出"; fi
