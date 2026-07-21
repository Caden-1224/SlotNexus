# VoxOrchestra —— 端侧轻量多进程通信与推理全离线大模型语音交互系统

> 「语音输入 → 本地检索 → 大模型推理 → 语音输出」全链路离线闭环，通信、调度、节点运行时全部自研。

## 项目简介

面向华为昇腾、鲲鹏、瑞芯微 RK 系列等边缘计算平台，本项目以 RK3576（立创·泰山派 3M，4 GB 内存）作为验证平台，自研一套轻量化多进程通信与推理中间件，在其上构建端侧全离线的大模型语音交互系统。

- **端侧**：面向低资源边缘设备，覆盖昇腾、鲲鹏、RK 系列等平台；当前以 RK3576（4 GB 内存 + 6 TOPS NPU）作为真机验证平台；
- **轻量**：以 4 GB 内存为约束设计——进程级隔离、有界队列背压、无向量库常驻开销、默认构建零厂商 SDK 依赖；实际内存驻留能力以板卡实测为准；
- **多进程**：Gateway、Unit Manager、Session 与各模型节点独立进程运行，故障边界清晰，节点按需启停、可独立替换；
- **通信与推理中间件**：ZMQ 多模式通信、TCP 网关、任务调度、Node 运行时与 Backend 契约全部独立实现，系统级依赖仅 ZeroMQ 与 nlohmann-json；
- **全离线**：不依赖公网与云端，适用于无网络、隐私敏感的部署环境；
- **大模型语音交互**：统一编排 ASR、本地 RAG、RKLLM 与 TTS，构建"语音输入 → 语音输出"全离线闭环（Mock 阶段五节点链路已跑通，真实链路板卡阶段逐级接入）。

系统以**单机多进程**为边界：不涉及跨主机集群、注册中心或故障转移；控制面 RPC（deadline + 结构化错误）与数据面异步流（有界、可取消）分离，外部客户端只访问 TCP 网关。

## 项目背景

端侧设备（树莓派、昇腾、瑞芯微 RK 系列等）处于低资源环境：算力分散、内存与通信受限、模型各自为政。把 ASR、LLM、TTS 直接串联会让业务代码同时承担模型调用、Socket、线程、超时、取消和退出逻辑，产生五类问题：

| 问题 | 后果 | 本项目对策 |
|---|---|---|
| 多个模型争抢 CPU / NPU / 内存 | 故障边界不清，一个模型崩溃拖垮全部 | 独立进程隔离，每个节点只占一份资源 |
| 模型加载与生命周期各不相同 | 无法为每次请求临时启停 | Unit Manager 统一任务生命周期（setup / exit） |
| 音频、token、PCM 是流式数据 | 不适合全部使用同步 RPC | 控制面 RPC + 数据面异步流分离 |
| 固定 `localhost` 端口互相耦合 | 节点无法独立替换 | 统一消息协议 + 动态 work_id 路由 |
| 并发请求缺少标识隔离 | 回复错位、晚到消息串入新会话 | work_id / request_id / session_id / generation 四级标识 |

本项目自研一套轻量化**多进程通信与推理中间件**——通信基座、任务调度、Node 运行时与后端契约全部独立实现，系统级依赖仅 ZeroMQ 与 nlohmann-json：统一 Node Runtime 承载所有模型节点，控制面与数据面分离，节点按需启停、可独立替换，可迁移至昇腾、鲲鹏、RK 系列等边缘平台复用。

## 项目架构图

```text
  Voice Client（WAV / 麦克风 / 文本）
        │  TCP + NDJSON
        ▼
   ┌───────────┐    ┌──────────────┐
   │  Gateway  │───▶│ Unit Manager │   setup / taskinfo / exit
   └───────────┘    └──────┬───────┘   （控制面 REQ/REP + deadline）
                           │
                    ┌──────▼───────┐
                    │   Session    │   work_id / request_id / session_id
                    └──────┬───────┘   取消传播与晚到消息过滤
   ┌───────────────────────┼───────────────────────┐
   ▼                       ▼                       ▼
 ASR Node ─▶ RAG Node ─▶ (L2/L3) LLM Node ─▶ TTS Node ─▶ WAV / ALSA
   └──────────── 数据面：ZeroMQ 异步流（有界、可取消） ────────────┘
```

### 三个平面

| 平面 | 内容 | 模式 | 关键约束 |
|---|---|---|---|
| 控制面 | setup / cancel / taskinfo / exit | REQ/REP RPC | deadline、结构化错误、幂等语义 |
| 数据面 | 音频帧 / ASR 结果 / token / PCM | PUB/SUB 或 PUSH/PULL | 异步、有界、可取消，不能无限堆积 |
| 外部接入 | 用户请求、流式响应 | TCP + NDJSON | 半包、粘包、超长帧、慢客户端 |

### 统一消息与标识符

统一消息为版本化 JSON 信封（`MessageEnvelope`）：`version / work_id / request_id / session_id / type / index / timestamp_ms / payload / finish / error`。四个标识符解决不同问题：

| 标识符 | 隔离粒度 | 典型场景 |
|---|---|---|
| `work_id` | 任务实例 | 一次 setup 起的整个任务生命周期 |
| `request_id` | 一次调用 | 单次 inference / cancel，回复按它归位 |
| `session_id` | 多轮会话 | 多轮对话上下文关联 |
| generation | 取消后的代际 | 取消后旧 token / PCM 直接丢弃，不串入新会话 |

## 项目设计方案

### 1. 通信基座：ZMQ 多模式通信中间件

- 统一封装 RPC / PUB-SUB / PUSH-PULL 三种通信策略（`libs/transport`），业务层按场景选择、调用方式一致，网络细节对业务屏蔽；
- **设计要点**：RPC 带 deadline 与结构化错误，超时自动重建连接保证控制面可用性；PUB-SUB 带订阅握手避免慢订阅者丢包；PUSH-PULL 用于任务分发——所有等待都有超时，不存在无限阻塞；
- 轻量序列化：版本化 JSON 信封，长度上限 1 MiB，编解码两端双重校验。

### 2. 网络接入：主从 Reactor TCP 框架

- epoll 事件循环（EventLoop / Channel / Poller），连接生命周期归属单一 loop 线程，主从 Reactor 分层；
- **设计要点**：NDJSON 增量解帧覆盖半包 / 粘包 / 超长帧 / 慢客户端写缓冲上限；所有连接回调都在 loop 线程执行，避免跨线程竞争；
- 多协议网关（`edge_gateway`）：TCP 接入 + ZMQ 控制面转发，外部用户与内部业务节点解耦。

### 3. 任务调度框架：Unit Manager 与 Node Runtime

- work_id 全局分配与路由（`TaskRegistry`），setup / inference / cancel / taskinfo / exit 状态机（`TaskChannel`），重复调用幂等；
- **设计要点**：控制面 RPC 服务注册与指令路由；轻量内存 KV 存储任务元信息，线程安全查询；任务实例交错 20 轮 E2E 无跨流；
- 数据面通道有界：容量、超时、关闭协议明确，防止慢消费者耗尽内存。

### 4. Node 业务层：标准化 Backend 契约

- **任务管理（类似线程）**：单任务实例内模型加载、推理与流式输出回调；
- **服务层控制（类似进程）**：自定义实现 setup 等接口，节点生命周期统一管理，节点间通过消息订阅交互；
- **设计要点**：五类可替换后端（`IAsrBackend / IRetriever / ILlmBackend / ITtsBackend / IAudioSink`）与统一事件（partial / final / token / pcm / done）；Node 外壳只依赖接口，默认构建全部使用确定性 Fake，真实后端按需接入——这是硬件接入的唯一变化点。

### 5. 语音交互链路：ASR → 分级 RAG → LLM → TTS

- **ASR**：流式识别，逐帧 partial、末帧 final（板卡阶段接入流式 Zipformer 识别框架；当前为确定性 Fake）；
- **分级 RAG**：L0 紧急控制（规则命中，绕过 LLM）/ L1 高置信事实直答 / L2 复杂问题带上下文 / L3 闲聊不注入伪知识；BM25 检索，可解释、可测试、无向量库常驻开销（当前为确定性 Fake 检索器，真实 BM25 与 JSONL 知识库为下一阶段）；
- **LLM**：DeepSeek-R1-Distill-Qwen-1.5B W4A16 预转换模型作为首个上游基线（板卡阶段），流式 token 回调与取消过滤；
- **TTS**：离线语音合成，消息/音频队列消除卡顿，当前输出 WAV 文件，板卡阶段接入声卡输出；
- **会话编排**：Idle → Listening → Routing → Thinking → Speaking 状态机与 generation 晚到过滤为当前开发阶段目标（见状态表）；已落地部分为节点级协作式取消与超时。

> 性能指标（时延、吞吐、内存占用）只以板卡实测为准，实测数据与方法记录于 `artifacts/`。

## 技术栈

| 技术 | 用在哪 |
|---|---|
| Linux / C++17 | 全链路实现语言：进程、线程、epoll 事件驱动 |
| ZeroMQ | 控制面 RPC 与数据面流的通信底座 |
| epoll 主从 Reactor | TCP 网关连接管理，连接生命周期一线程归属 |
| CMake + CTest | 根级构建与测试（当前 19 个测试） |
| Shell 脚本 | 演示、板卡体检与无硬件依赖验收 |

**应用场景**：无公网的全离线部署（工业、车载、机器人等边缘环境）；医疗、金融等隐私敏感场景；端侧语音交互与边缘智能应用。

## 当前状态

| 能力 | 状态 | 说明 |
|---|---|---|
| 仓库骨架、根级 CMake/CTest | ✅ | 空目录可复现构建，CTest 19/19 通过 |
| 统一消息信封 MessageEnvelope | ✅ | 版本化 JSON，1 MiB 上限，结构化错误码 |
| ZMQ 多模式通信 | ✅ | RPC（deadline）/ PUB/SUB（订阅握手）/ PUSH/PULL，均含超时与退出测试 |
| TCP 网关与 NDJSON 解帧 | ✅ | epoll 主从 Reactor，半包/粘包/超长帧/慢客户端处理 |
| Unit Manager / Node Runtime | ✅ | TaskChannel 状态机，Echo 三进程 E2E，双任务交错 20 轮无跨流 |
| 后端契约与确定性 Fake | ✅ | 五类接口 + 统一事件；ASR/RAG/LLM/TTS 以真实进程运行，TTS 产出 WAV |
| Session 编排、分级 RAG、取消与晚到过滤 | 🚧 进行中 | 下一阶段：L0-L3 路由、JSONL/BM25、有界队列、generation |
| 真实硬件后端（sherpa-onnx / RKLLM / SummerTTS / ALSA） | ⏳ 板卡阶段 | 默认构建关闭，仅 `VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=ON` 时接入 |
| 泰山派 3M 全真实链路 | ⏳ | 上游模型与运行库基线验证 → 项目 Backend → 全链路，逐级验证 |

## 快速开始

要求：WSL / Linux，CMake ≥ 3.22，C++17 编译器，libzmq3-dev（4.3.x）与 nlohmann-json3-dev（3.10.x）。默认构建**不依赖** NPU SDK、厂商 Runtime 或声卡。

```bash
cmake --preset wsl-debug
cmake --build --preset wsl-debug -j8
ctest --preset wsl-debug        # 19 个测试，全部通过
```

无硬件依赖可用 `scripts/check_no_hw_deps.sh` 逐二进制验收（ldd 检查 rkllm / sherpa / onnx / asound 等链接）。

## 演示（Mock 全链路）

```bash
scripts/demo_mock_chain.sh
```

一键拉起五节点 + Manager + 网关，展示 work_id 轮转路由、逐节点推理输出、TTS 产出的 WAV 与 SIGTERM 优雅退出；日志与音频落在 `/tmp/voxorchestra-demo/`。

单条协议交互（手动探测）：

```bash
python3 scripts/gateway_probe.py 9100 '{"version":1,"type":"setup","request_id":"s-1"}'
# → {"version":1,"work_id":"w-0","type":"ack",...}

python3 scripts/gateway_probe.py 9100 \
  '{"version":1,"type":"inference","work_id":"w-1","request_id":"r-1","payload":{"text":"3"}}'
# → 逐帧 partial 汇总后的 final 文本
```

## 设计约定

| 项 | 约定 |
|---|---|
| 节点端口 | echo `19200` / asr `19201` / rag `19202` / llm `19203` / tts `19204` |
| 网关端口 | `9100` |
| 音频格式 | 16 kHz 单声道 16-bit，20 ms 帧（320 采样） |
| 帧上限 | 单帧 1 MiB（解帧器与信封双重限制，超限断开） |
| 构建目录 | `build-wsl` / `build-taishanpi3m` 等按平台命名，不提交 |

## 项目结构

```text
libs/        通用库：common / protocol / transport / network / task_registry / runtime / rag / session
backends/    可替换实现：fake（默认）/ sherpa_onnx / rkllm / summer_tts / alsa
apps/        独立进程：edge_gateway / unit_manager / session_node / asr_node / rag_node / llm_node / tts_node / voice_cli
tests/       unit / contract / integration / e2e / fault
deploy/      部署：docker / systemd / taishanpi3m
config/      运行配置：mock / taishanpi3m
data/        知识库与固定输入：knowledge / fixtures
scripts/     演示与验收脚本
artifacts/   工程记录：环境、版本链与验收证据
docs/        设计文档（随开发补齐）
```

## 测试与证据

- 单元测试：协议、解帧、Reactor、任务注册表、运行时状态机、五类 Fake 契约；
- 集成测试：ZMQ 三模式真实收发、TCP 服务器、网关、三进程 Echo E2E、五节点 E2E；
- 证据目录 `artifacts/`：按能力记录命令、版本、测试、失败与缺陷；
- 版本链（板卡 → 镜像 → 内核 → NPU 驱动 → Runtime → 模型）在板卡接入后记录于 `artifacts/environment-preflight/versions.txt`。

## 第三方组件、模型与许可

- `THIRD_PARTY_NOTICES.md`：第三方组件登记与许可边界（当前系统依赖 ZeroMQ 与 nlohmann-json，均以系统包引入、不打包进仓库）；
- `models/README.md`：模型获取与校验方式（模型文件不入库）；
- `third_party/README.md`：第三方源码/SDK 获取说明（源码不入库）。
