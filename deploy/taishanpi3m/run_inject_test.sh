#!/bin/bash
# 板端核验：真机注入测试——取消 / 超时 / 错误输入。
# 进程组同 run_real_wav_chain.sh，llm_node 额外 --infer-timeout-ms 8000
# （思考未闭合即触发节点级超时路径）。注入序列：
#   1. 非法 JSON        → gateway 拒绝
#   2. 不存在的 WAV     → 会话错误（不崩）
#   3. 推理中 cancel    → cancelled 响应
#   4. 节点推理超时     → 会话 error（节点 kTimeout）
#   5. taskinfo 收尾   → 会话仍健康可查询
# 用法：板端执行。输出 /tmp/inject-test/。依赖根可用环境变量覆盖
# （与 run_real_wav_chain.sh 一致）。
set -u
DEPLOY_ROOT=${SLOTNEXUS_DEPLOY_ROOT:-$HOME/workspace/slotnexus-runtime}
RKLLM_ROOT=${SLOTNEXUS_RKLLM_ROOT:-$HOME/workspace/upstream_rkllm/rknn-llm/rkllm-runtime/Linux/librkllm_api}
SHERTA_ROOT=${SLOTNEXUS_SHERTA_ROOT:-$HOME/workspace/upstream_rkllm/sherpa-root}
MELOTTS_ROOT=${SLOTNEXUS_MELOTTS_ROOT:-$HOME/workspace/upstream_melotts}
cd "$DEPLOY_ROOT" || exit 1
export LD_LIBRARY_PATH="$RKLLM_ROOT/aarch64:$SHERTA_ROOT/build/lib:$SHERTA_ROOT/build/_deps/onnxruntime-src/lib:$MELOTTS_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
OUT=/tmp/inject-test
rm -rf "$OUT"
mkdir -p "$OUT/tts-node" "$OUT/session-out"
for p in edge_gateway unit_manager session_node asr_node llm_node tts_node; do pkill -TERM -x "$p" 2>/dev/null; done
sleep 0.5

./build-taishanpi3m-hw/apps/asr_node/asr_node \
 --listen tcp://127.0.0.1:19201 --config config/taishanpi3m/session.json \
 --events tcp://127.0.0.1:19421 --events-sync tcp://127.0.0.1:19422 \
 --infer-timeout-ms 30000 > "$OUT/asr.log" 2>&1 &
./build-taishanpi3m-hw/apps/llm_node/llm_node \
 --listen tcp://127.0.0.1:19203 --config config/taishanpi3m/session.json \
 --events tcp://127.0.0.1:19431 --events-sync tcp://127.0.0.1:19432 \
 --infer-timeout-ms 8000 > "$OUT/llm.log" 2>&1 &
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

PROBE="python3 scripts/gateway_probe.py 9100"

echo "== 1. 非法 JSON =="
$PROBE 'garbage' 10

echo "== 2. setup =="
$PROBE '{"version":1,"type":"setup","request_id":"s-0"}' 60

echo "== 3. 错误输入：不存在的 WAV =="
$PROBE '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-badwav","payload":{"mode":"wav","wav":"no_such.wav"}}' 30

echo "== 4. 取消：L3 长思考 prompt 推理发起 3s 后 cancel =="
$PROBE '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-cancel","payload":{"mode":"text","text":"请详细推导量子力学中薛定谔方程的数学过程，并解释其物理意义和实验验证方法"}}' 120 &
PROBE_PID=$!
sleep 3
$PROBE '{"version":1,"type":"cancel","work_id":"w-0","request_id":"c-1"}' 10
wait $PROBE_PID

echo "== 5. 节点推理超时：L3 长思考 prompt 触发 llm_node 8s 超时 =="
$PROBE '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-timeout","payload":{"mode":"text","text":"请详细推导量子力学中薛定谔方程的数学过程，并解释其物理意义和实验验证方法"}}' 60

echo "== 6. taskinfo 收尾（会话仍健康）=="
$PROBE '{"version":1,"type":"taskinfo","work_id":"w-0","request_id":"t-1"}' 30

echo "== 7. 输出残留检查 =="
ls -la "$OUT/session-out" "$OUT/tts-node" 2>/dev/null | grep -v "^total\|^d" || echo "（无输出文件：取消/错误路径未产出音频，符合预期）"

echo "== 日志尾部 =="
for f in session llm manager gateway; do echo "--- $f.log ---"; tail -4 "$OUT/$f.log"; done

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
