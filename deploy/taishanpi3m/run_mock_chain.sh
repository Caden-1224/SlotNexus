#!/usr/bin/env bash
# 泰山派 3M 板端运行：会话编排链（gateway → manager → session_node）。
#
# 拓扑：client --TCP--> edge_gateway --ZMQ--> unit_manager --ZMQ--> session_node
# 与 WSL Mock 同一二进制同一配置，板端 aarch64 原生构建产物在
# build-taishanpi3m/（先执行 deploy/taishanpi3m/build.sh）。
#
# 用法：bash deploy/taishanpi3m/run_mock_chain.sh
#   前台演示：启动三进程 → 冒烟验证（setup + 四类路由各一次）→ SIGTERM 优雅收尾；
#   日志与 WAV 输出落在 /tmp/voxorchestra-session/。
#   板端常驻：末尾 sleep 改 while 即可（SIGTERM 由 systemd/手动信号处理）。
set -u
cd "$(dirname "$0")/../.."
B=build-taishanpi3m
CFG=config/taishanpi3m/session.json
OUT=/tmp/voxorchestra-session
mkdir -p "$OUT"

if [ ! -x "$B/apps/session_node/session_node" ]; then
  echo "未找到板端构建产物，请先执行：bash deploy/taishanpi3m/build.sh"
  exit 1
fi

echo "== 清理残留进程 =="
for p in edge_gateway unit_manager session_node; do
  pkill -TERM -x "$p" 2>/dev/null
done
sleep 0.3
rm -f "$OUT"/*.log "$OUT"/*.wav

echo "== 启动三进程（日志见 $OUT/）=="
"$B/apps/session_node/session_node" \
  --listen tcp://127.0.0.1:19210 \
  --config "$CFG" \
  --output-dir "$OUT" \
  --fixture-dir data/fixtures > "$OUT/session.log" 2>&1 &
"$B/apps/unit_manager/unit_manager" \
  --listen tcp://127.0.0.1:19100 \
  --node tcp://127.0.0.1:19210 > "$OUT/manager.log" 2>&1 &
"$B/apps/edge_gateway/edge_gateway" > "$OUT/gateway.log" 2>&1 &
sleep 0.8

echo "== 启动日志 =="
cat "$OUT/session.log"
echo "--- manager.log ---"
cat "$OUT/manager.log"
echo "--- gateway.log ---"
cat "$OUT/gateway.log"

echo "== 冒烟验证 =="
probe() {  # probe <名称> <JSON>
  echo "-- $1"
  python3 scripts/gateway_probe.py 9100 "$2" \
    | python3 -c "
import sys,json
r=json.load(sys.stdin)
p=r.get('payload',{})
print(' ', r.get('request_id'), '->', 'type='+r['type'], 'status='+str(p.get('status','-')), 'route='+str(p.get('route','-')), 'pcm='+str(p.get('pcm_frames','-')))"
}
probe "setup（分配 work_id）" '{"version":1,"type":"setup","request_id":"s-1"}'
probe "L1 直答（16kHz 音频格式）" '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-l1","payload":{"mode":"text","text":"16kHz 音频格式"}}'
probe "L3 闲聊（走 Fake LLM）" '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-l3","payload":{"mode":"text","text":"你好 今天天气怎么样"}}'
probe "固定 WAV 全链路" '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-wav","payload":{"mode":"wav","wav":"voice.wav"}}'
probe "taskinfo（会话状态）" '{"version":1,"type":"taskinfo","work_id":"w-0","request_id":"t-1"}'
probe "exit（释放任务）" '{"version":1,"type":"exit","work_id":"w-0","request_id":"e-1"}'

echo "== WAV 输出 =="
ls -la "$OUT"/*.wav 2>/dev/null || echo "（本次未产生 WAV 输出文件）"

echo "== SIGTERM 优雅收尾 =="
for p in edge_gateway unit_manager session_node; do
  pkill -TERM -x "$p"
done
sleep 0.6
if pgrep -x edge_gateway >/dev/null || pgrep -x unit_manager >/dev/null || pgrep -x session_node >/dev/null; then
  echo "仍有进程存活！"
  exit 1
fi
echo "三进程已全部退出"
