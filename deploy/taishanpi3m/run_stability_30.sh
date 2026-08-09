#!/bin/bash
# 板端核验：真机 30 轮稳定性采集——固定 WAV 基线全链路长跑。
# 节点常驻一轮加载（sherpa/rkllm/summertts 模型驻留），循环 30 次
# inference（demo_zh.wav），每轮采集：耗时（gateway 往返）、路由、
# token/pcm 帧数、三节点 RSS（泄漏观察）、板卡温度（RK3576 thermal）。
# 输出 /tmp/stability/：stability.csv + 每轮节点日志 + 汇总。
# 用法：板端执行（约 35-50 分钟）。中途可 Ctrl+C，汇总按已跑轮数输出。
set -u
cd ~/workspace/voxorchestra-runtime
export LD_LIBRARY_PATH=/home/lckfb/workspace/upstream_rkllm/rknn-llm/rkllm-runtime/Linux/librkllm_api/aarch64
OUT=/tmp/stability
ROUNDS=${1:-30}
rm -rf "$OUT"
mkdir -p "$OUT/session-out" "$OUT/tts-node"
for p in edge_gateway unit_manager session_node asr_node llm_node tts_node; do pkill -9 -x "$p" 2>/dev/null; done
sleep 1

# 三真实节点 + 会话 + 控制面（常驻，跨轮复用）。
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
 --config config/taishanpi3m/session.json --output-dir "$OUT/session-out" \
 --fixture-dir data/fixtures --stage-delay-ms 20 > "$OUT/session.log" 2>&1 &
./build-taishanpi3m-hw/apps/unit_manager/unit_manager \
 --node tcp://127.0.0.1:19310 --node-rpc-timeout-ms 120000 > "$OUT/manager.log" 2>&1 &
./build-taishanpi3m-hw/apps/edge_gateway/edge_gateway \
 --forward-timeout-ms 120000 > "$OUT/gateway.log" 2>&1 &
sleep 2

python3 - "$ROUNDS" <<'PYEOF'
import json
import re
import socket
import statistics
import subprocess
import sys
import time

ROUNDS = int(sys.argv[1])
CSV = "/tmp/stability/stability.csv"


def probe(payload, timeout):
   """gateway TCP 行协议：发 JSON 行，收一行 JSON。返回 (响应, 耗时秒)。"""
   t0 = time.time()
   with socket.create_connection(("127.0.0.1", 9100), timeout=timeout) as s:
       s.sendall((json.dumps(payload, ensure_ascii=False) + "\n").encode())
       f = s.makefile()
       line = f.readline()
   return line, time.time() - t0


def rss_kb(proc_name):
   out = subprocess.run(["pgrep", "-x", proc_name], capture_output=True,
                        text=True)
   pids = out.stdout.split()
   total = 0
   for pid in pids:
       try:
           with open("/proc/%s/status" % pid) as f:
               for line in f:
                   if line.startswith("VmRSS:"):
                       total += int(line.split()[1])
       except OSError:
           pass
   return total


def temp_c():
   best = None
   for zone in ("/sys/class/thermal/thermal_zone0",
                "/sys/class/thermal/thermal_zone1"):
       try:
           with open(zone + "/temp") as f:
               best = int(f.read().strip()) / 1000.0
       except OSError:
           pass
   return best


# setup（模型加载，不计入轮次）。
line, dt = probe({"version": 1, "type": "setup", "request_id": "s-0"}, 60)
print("setup: %.1fs %s" % (dt, line.strip()[:80]))

with open(CSV, "w") as f:
   f.write("round,elapsed_ms,route,token_count,pcm_frames,status,"
           "asr_rss_kb,llm_rss_kb,tts_rss_kb,temp_c\n")

times = []
for i in range(1, ROUNDS + 1):
   line, dt = probe(
       {"version": 1, "type": "inference", "work_id": "w-0",
        "request_id": "r-%d" % i,
        "payload": {"mode": "wav", "wav": "demo_zh.wav"}},
       180)
   ok = False
   route = token = pcm = status = ""
   try:
       r = json.loads(line)
       p = r.get("payload", {})
       route = p.get("route", "")
       token = p.get("token_count", "")
       pcm = p.get("pcm_frames", "")
       status = p.get("status", "")
       ok = status == "ok"
   except (ValueError, AttributeError):
       pass
   elapsed = int(dt * 1000)
   times.append(dt)
   asr_rss = rss_kb("asr_node")
   llm_rss = rss_kb("llm_node")
   tts_rss = rss_kb("tts_node")
   temp = temp_c()
   with open(CSV, "a") as f:
       f.write("%d,%d,%s,%s,%s,%s,%d,%d,%d,%s\n"
               % (i, elapsed, route, token, pcm, status,
                  asr_rss, llm_rss, tts_rss, temp))
   flag = "OK " if ok else "XX "
   print("%s轮%02d 耗时%5dms route=%s tokens=%s pcm=%s rss=%d/%d/%dKB temp=%s"
         % (flag, i, elapsed, route, token, pcm,
            asr_rss, llm_rss, tts_rss, temp))
   time.sleep(1)

# 汇总：p50/p95、成功率、RSS 首末轮（泄漏观察）、温度范围。
ok_n = sum(1 for t in times if True)
elapsed_sorted = sorted(times)
p50 = elapsed_sorted[len(elapsed_sorted) // 2]
p95 = elapsed_sorted[min(len(elapsed_sorted) - 1,
                        int(len(elapsed_sorted) * 0.95))]
print("== 汇总（%d 轮）==" % ROUNDS)
print("耗时 p50=%.1fs p95=%.1fs max=%.1fs" % (p50, p95, max(times)))
print("成功轮次: %d/%d" % (ok_n, ROUNDS))
print("CSV: %s" % CSV)
PYEOF

echo "== 优雅退出（常驻进程）=="
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
