# 演示提纲

## 60 秒项目介绍

SlotNexus 是面向低资源 Linux 边缘设备的单机多进程通信与推理中间件。
它把 Gateway、任务管理、Session、ASR、LLM 和 TTS 分成独立进程，用
版本化消息、带 deadline 的控制面 RPC 和异步数据面事件统一模型生命周期；
五类可替换后端（ASR / 检索 / LLM / TTS / 音频输出）只按同一套契约接入，
默认全 Fake 构建即可在无 NPU、无声卡的环境上做确定性回归。
首个端到端应用是离线语音闭环：默认 x86 构建不接触任何厂商 SDK；
泰山派 3M 硬件构建接入 sherpa-onnx、RKLLM、SummerTTS 和 ALSA。
该链路已完成固定 WAV、现场麦克风、故障注入和 30 轮全真实稳定性，
30/30 成功（该 30 轮取自上一代 1.5B LLM 基线，更换 LLM 后全链路未重跑）。
当前主要边界是 LLM 生成速度决定的首音频延迟（Qwen3.5-0.8B 单轮约 8–11 秒、
TTFT 约 3.0 秒）、Runtime 长时劣化、固定 3 秒麦克风窗口以及 REP 推理期间
cancel 不能插队。

## 5 分钟演示顺序

1. 展示版本链和 `check_deployment.sh` 预检结果，说明模型与 SDK 不在仓库。
2. 执行 `start.sh`，查看六个 PID 和 `setup.log` 的 ack。
3. 用固定 WAV 发起一次请求，展示 route、token_count、PCM 帧、队列峰值和
   16 kHz WAV 输出；不要以生成文本质量替代链路成功判断。
4. 展示现场麦克风记录和故障注入摘要，说明麦克风为固定 3 秒窗口。
5. 展示 30 轮 p50/p95、RSS、温度和 Fake/Mock 扫描结果。
6. 执行 `stop.sh`，展示六进程和 PID 文件均无残留。

演示前不得并行运行旧诊断脚本；先按六个精确进程名执行 `pkill -9`。
