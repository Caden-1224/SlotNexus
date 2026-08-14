# 部署与运行排查

## 先做部署预检

```bash
bash deploy/taishanpi3m/check_deployment.sh
```

预检逐项确认六个程序、板端配置、知识库、三个模型、RKLLM Runtime、
sherpa-onnx 和 ONNX Runtime。失败时先修正缺失路径或 `ldd` 的 unresolved
依赖，不要跳过预检直接启动。

## 启动失败或端口被占用

只清理六个精确进程名，再启动：

```bash
pkill -9 -x edge_gateway 2>/dev/null || true
pkill -9 -x unit_manager 2>/dev/null || true
pkill -9 -x session_node 2>/dev/null || true
pkill -9 -x asr_node 2>/dev/null || true
pkill -9 -x llm_node 2>/dev/null || true
pkill -9 -x tts_node 2>/dev/null || true
bash deploy/taishanpi3m/start.sh
```

日志和 PID 默认位于 `/tmp/voxorchestra-runtime`。启动或 setup 失败会自动
回滚，日志保留。不要同时运行旧诊断脚本和发布入口。

## 动态库错误

- `librkllmrt.so`：检查 `VOXORCHESTRA_RKLLM_ROOT/aarch64/`；
- `libsherpa-onnx-c-api.so`：检查 `VOXORCHESTRA_SHERTA_ROOT/build/lib/`；
- ONNX Runtime：检查 sherpa 构建目录的 `_deps/onnxruntime-src/lib/`；
- 使用 `ldd <binary>` 查找 `not found`，不要把 `.so` 复制进仓库。

## 推理变慢

DeepSeek-R1 的 30–60 秒回答属于已知边界。若随轮次继续劣化，同时采集：

```bash
grep -E 'VmRSS|VmHWM' /proc/<pid>/status
cat /sys/class/thermal/thermal_zone*/temp
cat /sys/kernel/debug/rknpu/load 2>/dev/null || true
```

RKLLM Runtime 1.2.0 / RKNPU 0.9.8 的长时劣化尚未修复；不要用 Fake 输出
替代，也不要把重新启动后的最好轮次混入同一统计。

## cancel 看似延迟

同一 ZeroMQ REP 处理函数正在推理时不能接收插队的 cancel。当前保证是：
Backend 协作式取消、generation 过滤晚到 token/PCM，以及 L0 停止/取消规则
绕过 LLM。需要真正抢占式取消时，应改造控制通道或推理执行模型，而不是
调小客户端超时伪装完成。

## 麦克风与音频

麦克风入口当前固定采集 3 秒，不是 VAD 常驻流。先用固定 WAV 复现模型链，
再检查 `arecord -l`、声卡设备和录音 RMS。SummerTTS 输出为 16 kHz mono
S16；ES8323 使用 `plughw:0,0` 时支持 16 kHz，其他采样率可能触发回退和
重采样。发布入口默认保留 WAV 作为可复现输出，ALSA 用单独诊断入口核验。

## 停止与残留

```bash
bash deploy/taishanpi3m/stop.sh
ps -C edge_gateway -C unit_manager -C session_node -C asr_node -C llm_node -C tts_node
```

`stop.sh` 可重复执行。它先 SIGTERM，最多等待 20 秒，再 SIGKILL；PID 文件
会在停止后删除。
