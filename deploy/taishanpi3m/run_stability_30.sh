#!/bin/bash
# 板端核验：真机 30 轮稳定性采集——固定 WAV 基线全链路重复可用性。
# 每轮完整重建六进程（板端 NPU 驱动持续运行会劣化：rkllm 生成中途停滞
# 超时，重启板卡才恢复；进程级重建规避劣化累积）+ 每轮前释放文件缓存
# （4GB 板内存吃紧）+ 超时放宽（llm 节点 120s）。每轮采集：gateway 往返
# 耗时、路由、token/pcm 帧数、三节点 RSS、板卡温度。
# 输出 /tmp/stability/：stability.csv + 每轮日志目录 + 汇总。
# 依赖根可用环境变量覆盖（与 run_real_wav_chain.sh 一致）。
# 用法：板端执行（约 45-60 分钟）。中途 Ctrl+C 可停，CSV 已落盘。
set -u
DEPLOY_ROOT=${SLOTNEXUS_DEPLOY_ROOT:-$HOME/workspace/slotnexus-runtime}
RKLLM_ROOT=${SLOTNEXUS_RKLLM_ROOT:-$HOME/workspace/upstream_rkllm/rknn-llm/rkllm-runtime/Linux/librkllm_api}
SHERTA_ROOT=${SLOTNEXUS_SHERTA_ROOT:-$HOME/workspace/upstream_rkllm/sherpa-root}
MELOTTS_ROOT=${SLOTNEXUS_MELOTTS_ROOT:-$HOME/workspace/upstream_melotts}
cd "$DEPLOY_ROOT" || exit 1
export LD_LIBRARY_PATH="$RKLLM_ROOT/aarch64:$SHERTA_ROOT/build/lib:$SHERTA_ROOT/build/_deps/onnxruntime-src/lib:$MELOTTS_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
ROUNDS=${1:-30}
BASE=/tmp/stability
rm -rf "$BASE"
mkdir -p "$BASE"

# 单轮进程组：起六进程 → setup → 固定 WAV 推理 → 采集 → 优雅退出。
run_round() {
 local i=$1
 local OUT="$BASE/r$i"
 mkdir -p "$OUT/session-out" "$OUT/tts-node"
 for p in edge_gateway unit_manager session_node asr_node llm_node tts_node; do pkill -9 -x "$p" 2>/dev/null; done
 echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null 2>&1
 sleep 1
 ./build-taishanpi3m-hw/apps/asr_node/asr_node \
   --listen tcp://127.0.0.1:19201 --config config/taishanpi3m/session.json \
   --events tcp://127.0.0.1:19421 --events-sync tcp://127.0.0.1:19422 \
   --infer-timeout-ms 30000 > "$OUT/asr.log" 2>&1 &
 ./build-taishanpi3m-hw/apps/llm_node/llm_node \
   --listen tcp://127.0.0.1:19203 --config config/taishanpi3m/session.json \
   --events tcp://127.0.0.1:19431 --events-sync tcp://127.0.0.1:19432 \
   --infer-timeout-ms 120000 > "$OUT/llm.log" 2>&1 &
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
   --net-setup-timeout-ms 60000 --net-rpc-timeout-ms 120000 \
   --config config/taishanpi3m/session.json --output-dir "$OUT/session-out" \
   --fixture-dir data/fixtures --stage-delay-ms 20 > "$OUT/session.log" 2>&1 &
 ./build-taishanpi3m-hw/apps/unit_manager/unit_manager \
   --node tcp://127.0.0.1:19310 --node-rpc-timeout-ms 120000 > "$OUT/manager.log" 2>&1 &
 ./build-taishanpi3m-hw/apps/edge_gateway/edge_gateway \
   --forward-timeout-ms 120000 > "$OUT/gateway.log" 2>&1 &
 sleep 2
 python3 - "$i" "$OUT" <<'PYEOF'
import json
import socket
import subprocess
import sys
import time

i, out_dir = sys.argv[1], sys.argv[2]
CSV = "/tmp/stability/stability.csv"


def probe(payload, timeout):
   t0 = time.time()
   with socket.create_connection(("127.0.0.1", 9100), timeout=timeout) as s:
       s.sendall((json.dumps(payload, ensure_ascii=False) + "\n").encode())
       line = s.makefile().readline()
   return line, time.time() - t0


def rss_kb(proc_name):
   out = subprocess.run(["pgrep", "-x", proc_name], capture_output=True,
                        text=True)
   total = 0
   for pid in out.stdout.split():
       try:
           with open("/proc/%s/status" % pid) as f:
               for line in f:
                   if line.startswith("VmRSS:"):
                       total += int(line.split()[1])
       except OSError:
           pass
   return total


def temp_c():
   for zone in ("/sys/class/thermal/thermal_zone0",
                "/sys/class/thermal/thermal_zone1"):
       try:
           with open(zone + "/temp") as f:
               return int(f.read().strip()) / 1000.0
       except OSError:
           pass
   return None


line, _ = probe({"version": 1, "type": "setup", "request_id": "s-0"}, 60)
try:
   line, dt = probe(
       {"version": 1, "type": "inference", "work_id": "w-0",
        "request_id": "r-%s" % i,
        "payload": {"mode": "wav", "wav": "demo_zh.wav"}}, 200)
   r = json.loads(line)
   p = r.get("payload", {})
   route = p.get("route", "?")
   token = p.get("token_count", "?")
   pcm = p.get("pcm_frames", "?")
   status = p.get("status", "?")
except (ValueError, AttributeError, OSError) as e:
   route = token = pcm = "?"
   status = "probe_err:" + str(e)
   dt = 200.0

asr_rss = rss_kb("asr_node")
llm_rss = rss_kb("llm_node")
tts_rss = rss_kb("tts_node")
temp = temp_c()
with open(CSV, "a") as f:
   f.write("%s,%d,%s,%s,%s,%s,%d,%d,%d,%s\n"
           % (i, int(dt * 1000), route, token, pcm, status,
              asr_rss, llm_rss, tts_rss, temp))
flag = "OK " if status == "ok" else "XX "
print("%s轮%s 耗时%5dms route=%s tokens=%s pcm=%s rss=%d/%d/%dKB temp=%s"
     % (flag, i, int(dt * 1000), route, token, pcm,
        asr_rss, llm_rss, tts_rss, temp))
PYEOF
 # 优雅退出（轮询等待，记录最长退出耗时）。
 for p in edge_gateway unit_manager session_node asr_node llm_node tts_node; do pkill -TERM -x "$p" 2>/dev/null; done
 T0=$(date +%s%N)
 ALIVE=1
 for n in $(seq 1 40); do
   ALIVE=0
   for p in edge_gateway unit_manager session_node asr_node llm_node tts_node; do
     pgrep -x "$p" > /dev/null && ALIVE=1
   done
   [ "$ALIVE" = 0 ] && break
   sleep 0.5
 done
 T1=$(date +%s%N)
 local EXIT_S=$(( (T1 - T0) / 1000000000 ))
 if [ "$ALIVE" = 0 ]; then
   echo "  退出耗时: ${EXIT_S}s（全部进程已退出）"
 else
   echo "  退出耗时: ${EXIT_S}s（仍有进程存活）"
 fi
}

echo "round,elapsed_ms,route,token_count,pcm_frames,status,asr_rss_kb,llm_rss_kb,tts_rss_kb,temp_c" > /tmp/stability/stability.csv
for i in $(seq 1 "$ROUNDS"); do
 run_round "$i"
done

echo "== 汇总（$ROUNDS 轮）=="
python3 - /tmp/stability/stability.csv <<'PYEOF'
import csv
import math
import statistics
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
print("CSV: /tmp/stability/stability.csv")
PYEOF
