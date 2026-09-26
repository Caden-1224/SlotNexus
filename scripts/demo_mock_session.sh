#!/bin/bash
# Session Mock 全链路演示：固定 WAV → Fake ASR → BM25 L0-L3 路由
# →（Fake LLM）→ 分句 → Fake TTS → WAV 输出。
#
# 进程拓扑：client --TCP--> edge_gateway --ZMQ--> unit_manager --ZMQ--> session_node
# 日志与输出落在 /tmp/slotnexus-session/。
#
# 演示内容：
#   1. 四条路由各走对路径（L0 控制 / L1 直答 / L2 带上下文 / L3 闲聊）；
#   2. 固定 WAV（data/fixtures/voice.wav）完整链路输出 PCM/WAV；
#   3. 取消传播：取消进行中的推理，随后新请求正常完成；
#   4. taskinfo 展示会话状态与队列统计；
#   5. SIGTERM 三进程优雅退出。
#
# 用法：scripts/demo_mock_session.sh [--stage-delay-ms N]
#   --stage-delay-ms 给各阶段注入人工延时（演示取消用，默认 20）。
set -u
cd "$(dirname "$0")/.."
B=build-wsl
OUT=/tmp/slotnexus-session
STAGE_DELAY=${1:-20}
if [ "$STAGE_DELAY" = "--stage-delay-ms" ]; then STAGE_DELAY=$2; fi
mkdir -p "$OUT"

echo "== 清理残留进程 =="
for p in edge_gateway unit_manager session_node; do
 pkill -TERM -x "$p" 2>/dev/null
done
sleep 0.3
rm -f "$OUT"/*.log "$OUT"/*.wav

echo "== 启动 session_node + unit_manager + edge_gateway（stage-delay=${STAGE_DELAY}ms）=="
"$B/apps/session_node/session_node" \
 --listen tcp://127.0.0.1:19210 \
 --config config/mock/session.json \
 --output-dir "$OUT" \
 --fixture-dir data/fixtures \
 --stage-delay-ms "$STAGE_DELAY" > "$OUT/session.log" 2>&1 &
"$B/apps/unit_manager/unit_manager" \
 --listen tcp://127.0.0.1:19100 \
 --node tcp://127.0.0.1:19210 > "$OUT/manager.log" 2>&1 &
"$B/apps/edge_gateway/edge_gateway" > "$OUT/gateway.log" 2>&1 &
sleep 0.8

echo "== 各进程启动日志 =="
cat "$OUT/session.log"
echo "--- manager.log ---"
cat "$OUT/manager.log"

echo "== setup：分配会话 work_id =="
python3 scripts/gateway_probe.py 9100 \
 '{"version":1,"type":"setup","request_id":"s-1"}' \
 | python3 -c "import sys,json; r=json.load(sys.stdin); print('setup ->', r['work_id'])"

probe() {  # probe <名称> <JSON>
 echo "-- $1"
 python3 scripts/gateway_probe.py 9100 "$2" \
   | python3 -c "
import sys,json
r=json.load(sys.stdin)
p=r.get('payload',{})
print('route=%s status=%s final_text=%s' % (p.get('route','-'), p.get('status','-'), p.get('final_text','(error: %s)' % r.get('error',{}).get('message','-'))))
print('  pcm_frames=%s token_count=%s wav=%s' % (p.get('pcm_frames','-'), p.get('token_count','-'), p.get('wav_path','-')))
"
}

echo "== 四类路由 =="
probe "L0 紧急控制：停止播放" \
 '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-l0","payload":{"mode":"text","text":"停止播放"}}'
probe "L1 事实直答：16kHz 音频格式" \
 '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-l1","payload":{"mode":"text","text":"16kHz 音频格式"}}'
probe "L2 复杂带上下文：生成过滤怎么实现" \
 '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-l2","payload":{"mode":"text","text":"生成过滤怎么实现"}}'
probe "L3 闲聊：你好 今天天气怎么样" \
 '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-l3","payload":{"mode":"text","text":"你好 今天天气怎么样"}}'

echo "== 固定 WAV 完整链路（data/fixtures/voice.wav → Fake ASR → ... → WAV）=="
probe "WAV 输入" \
 '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-wav","payload":{"mode":"wav","wav":"voice.wav"}}'

echo "== 取消传播 =="
# 说明：gateway/unit_manager 当前为同步 REQ/REP 转发，inference 在途时
# 管理器阻塞等待节点回复，cancel 请求会排在推理之后——这是控制面同步
# 模型的已知限制（议题：exit 与 inference 并发）。session_node
# 本身为异步 ROUTER 服务，inference 期间可并发收到 cancel/taskinfo/exit；
# 取消传播的完整验证见 tests/e2e/session_e2e_test 的直连场景与
# tests/unit/session_pipeline_test 的顽固后端晚到过滤测试。
echo "（取消传播通过 session_node 直连与管线单测验证，见上文说明）"

echo "== 任务统计（taskinfo）=="
probe "重新直答一次供 taskinfo 展示" \
 '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-again","payload":{"mode":"text","text":"16kHz 音频格式"}}'

echo "== taskinfo：会话状态与队列统计 =="
python3 scripts/gateway_probe.py 9100 \
 '{"version":1,"type":"taskinfo","work_id":"w-0","request_id":"t-1"}' \
 | python3 -c "
import sys,json
r=json.load(sys.stdin)
p=r['payload']
print('state=%s busy=%s route=%s' % (p.get('state'), p.get('busy'), p.get('route')))
print('text_queue_peak=%s pcm_queue_peak=%s dropped_sentences=%s' % (p.get('text_queue_peak'), p.get('pcm_queue_peak'), p.get('dropped_sentences')))
"

echo "== 输出文件（可拷到 Windows 播放）=="
echo "注意：Fake TTS 产出的是 500Hz 测试音（验证 PCM 链路正确性），"
echo "实际内容见上方各请求的 final_text；真实语音由 SummerTTS 合成。"
ls -la "$OUT"/*.wav 2>/dev/null | head -5

echo "== SIGTERM 优雅退出 =="
for p in edge_gateway unit_manager session_node; do
 pkill -TERM -x "$p"
done
sleep 0.6
LEFT=0
for p in edge_gateway unit_manager session_node; do
 if pgrep -x "$p" > /dev/null; then
   echo "进程仍存活：$p"
   LEFT=1
 fi
done
if [ "$LEFT" = "0" ]; then
 echo "三进程全部退出"
fi
exit "$LEFT"
