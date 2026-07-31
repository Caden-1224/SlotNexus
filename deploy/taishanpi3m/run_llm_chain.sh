#!/bin/bash
# 板端核验：gateway → manager → llm_node（rkllm 后端）固定 prompt 全链路。
# 用法：板端执行（LD_LIBRARY_PATH 指向 SDK aarch64 目录）。
set -u
cd ~/workspace/voxorchestra-runtime
export LD_LIBRARY_PATH=/home/lckfb/workspace/upstream_rkllm/rknn-llm/rkllm-runtime/Linux/librkllm_api/aarch64
OUT=/tmp/llm-chain
mkdir -p "$OUT"
for p in edge_gateway unit_manager llm_node; do pkill -TERM -x "$p" 2>/dev/null; done
sleep 0.5

./build-taishanpi3m-hw/apps/llm_node/llm_node --config config/taishanpi3m/session.json --infer-timeout-ms 60000 > "$OUT/llm.log" 2>&1 &
LLMPID=$!
./build-taishanpi3m-hw/apps/unit_manager/unit_manager --node tcp://127.0.0.1:19203 --node-rpc-timeout-ms 30000 > "$OUT/manager.log" 2>&1 &
./build-taishanpi3m-hw/apps/edge_gateway/edge_gateway --forward-timeout-ms 30000 > "$OUT/gateway.log" 2>&1 &
sleep 1.2

echo "== setup =="
python3 scripts/gateway_probe.py 9100 '{"version":1,"type":"setup","request_id":"s-0"}' 30

echo "== inference（固定 prompt，与 smoke 同款）=="
python3 scripts/gateway_probe.py 9100 '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-llm","payload":{"text":"你好，请用一句话介绍你自己。"}}' 60

echo "== taskinfo =="
python3 scripts/gateway_probe.py 9100 '{"version":1,"type":"taskinfo","work_id":"w-0","request_id":"t-1"}' 30

echo "== VmHWM / VmRSS / VmPeak（llm_node 峰值内存）=="
grep -E "VmHWM|VmRSS|VmPeak" /proc/$LLMPID/status

echo "== llm_node 启动日志 =="
cat "$OUT/llm.log"

echo "== 优雅退出 =="
for p in edge_gateway unit_manager llm_node; do pkill -TERM -x "$p"; done
sleep 0.8
pgrep -x edge_gateway > /dev/null && echo "仍有进程存活" || echo "全部进程已退出"
