# VoxOrchestra

基于 C++ 多进程推理框架的端侧全离线语音交互系统：在单台 RK3576（立创·泰山派 3M）Linux 设备上，用统一 Node Runtime、任务生命周期和流式消息编排 ASR、本地 RAG、RKLLM 与 TTS，完成无需公网的语音输入到语音输出闭环。

## 已完成功能

- **统一消息信封**：MessageEnvelope 版本化 JSON 编解码，1 MiB 上限，结构化错误码，全链路同一协议
- **通信基座**：ZeroMQ 三种模式——带 deadline 的 REQ/REP RPC、带订阅握手的 PUB/SUB、PUSH/PULL 任务分发
- **网络接入层**：epoll 事件循环（EventLoop/Channel/Poller）+ TCP/NDJSON 增量解帧，慢客户端写缓冲保护
- **任务运行时**：TaskChannel 状态机（setup/inference/cancel/taskinfo/exit），单流语义、协作式取消与超时、容量控制
- **控制面链路**：edge_gateway → unit_manager → 节点，work_id 全局分配与路由，Echo 演示后端
- **端到端验证**：双任务交错 20 轮无跨流，未知任务/取消/重复 exit 语义正确，三进程 SIGTERM 优雅退出（CTest 12/12）

## 快速开始

```bash
cmake --preset wsl-debug
cmake --build --preset wsl-debug
ctest --preset wsl-debug
```

## 演示（Echo 链路）

```bash
./build-wsl/apps/echo_node/echo_node &
./build-wsl/apps/unit_manager/unit_manager &
./build-wsl/apps/edge_gateway/edge_gateway &

# 客户端探测：setup 获得 work_id，再发推理
python3 scripts/gateway_probe.py 9100 '{"version":1,"type":"setup","request_id":"s-1"}'
python3 scripts/gateway_probe.py 9100 '{"version":1,"type":"inference","work_id":"w-0","request_id":"r-1","payload":{"text":"你好"}}'
```

## 项目结构

```text
libs/        通用库：common / protocol / transport / network / task_registry / runtime / rag / session
backends/    可替换实现：fake（默认）/ sherpa_onnx / rkllm / summer_tts / alsa
apps/        独立进程：edge_gateway / unit_manager / session_node / asr_node / rag_node / llm_node / tts_node / voice_cli
tests/       unit / contract / integration / e2e / fault
deploy/      部署：docker / systemd / taishanpi3m
artifacts/   工程记录：环境、版本链与验收证据
config/      运行配置：mock / taishanpi3m
data/        知识库与固定输入：knowledge / fixtures
```

## 路线图

- 四类 Backend 契约与 Fake 节点（ASR / RAG / LLM / TTS）
- Session 编排、请求队列与取消传播
- 真实硬件后端接入（sherpa-onnx / RKLLM / SummerTTS）
