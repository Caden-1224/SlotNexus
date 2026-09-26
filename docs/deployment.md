# 泰山派 3M 部署与进程生命周期

## 适用范围

本文定义全真实语音链路的板端启动与停止入口。部署包不包含模型、厂商
SDK、动态库、板卡地址或凭据；这些资源由部署环境提供。

## 部署包内容

发布源码包由 `git archive` 生成，不含 `.git/` 和构建缓存。运行相关内容：

| 内容 | 路径 |
|---|---|
| 六进程源码与公共库 | `apps/`、`libs/`、`backends/` |
| 板端配置 | `config/taishanpi3m/session.json` |
| 固定非隐私输入与知识库 | `data/fixtures/`、`data/knowledge/` |
| 构建/预检/启动/停止 | `deploy/taishanpi3m/{build,check_deployment,start,stop}.sh` |
| 版本、许可和发布证据 | `artifacts/`、`THIRD_PARTY_NOTICES.md` |

模型、SDK、`.so`、构建目录、原始日志、现场录音、凭据和板卡地址明确排除。

## 板端构建

先安装系统依赖：CMake、C++17 编译器、ZeroMQ、nlohmann-json 和 ALSA
开发包。硬件构建还需要在板端外部准备 sherpa-onnx、RKLLM Runtime、
SummerTTS 源码及三类模型：

```bash
export VOXORCHESTRA_SHERTA_ROOT=<sherpa-onnx 根目录>
export VOXORCHESTRA_RKLLM_ROOT=<librkllm_api 根目录>
export VOXORCHESTRA_SUMMERTTS_ROOT=<SummerTTS 根目录>
export VOXORCHESTRA_ASR_MODEL=<ASR 模型目录>
export VOXORCHESTRA_RKLLM_MODEL=<RKLLM 模型文件>
export VOXORCHESTRA_TTS_MODEL=<TTS 模型文件>
export VOXORCHESTRA_BUILD_JOBS=4
bash deploy/taishanpi3m/build.sh hardware
```

`build.sh` 即使检测到更多 CPU 也把并行度限制为 4。4 GB 板卡编译
SummerTTS 的 Eigen 模板代码时若内存紧张，应进一步降低为 1 或 2，不提高
上限。默认构建使用 `build.sh default`，并额外执行无硬件依赖门禁。

## 命令入口

```bash
export VOXORCHESTRA_RKLLM_ROOT=<librkllm_api 根目录>
export VOXORCHESTRA_SHERTA_ROOT=<sherpa-onnx 根目录>
bash deploy/taishanpi3m/start.sh
bash deploy/taishanpi3m/stop.sh
```

`start.sh` 使用 `nohup` 在后台启动六个服务，使其不依赖当前 SSH 终端，
并完成模型 `setup`。`stop.sh` 停止本次部署的服务，可重复执行。

## 环境与状态

启动入口使用以下环境变量：

| 变量 | 必需 | 用途 |
|---|---|---|
| `VOXORCHESTRA_RKLLM_ROOT` | 是 | 提供 `aarch64/librkllmrt.so` |
| `VOXORCHESTRA_SHERTA_ROOT` | 是 | 提供 sherpa-onnx 与 ONNX Runtime 动态库 |
| `VOXORCHESTRA_DEPLOY_ROOT` | 否 | 部署根目录，默认由脚本位置推导 |
| `VOXORCHESTRA_BUILD_DIR` | 否 | 硬件构建目录，默认 `build-taishanpi3m-hw` |
| `VOXORCHESTRA_CONFIG` | 否 | 板端配置，默认 `config/taishanpi3m/session.json` |
| `VOXORCHESTRA_RUN_DIR` | 否 | PID 与日志目录，默认 `/tmp/voxorchestra-runtime` |
| `VOXORCHESTRA_SETUP_TIMEOUT_SECONDS` | 否 | `setup` 等待秒数，默认 120 |

运行库搜索路径由 `VOXORCHESTRA_RKLLM_ROOT/aarch64`、
`VOXORCHESTRA_SHERTA_ROOT/build/lib` 和
`VOXORCHESTRA_SHERTA_ROOT/build/_deps/onnxruntime-src/lib` 推导，并保留
调用者已有的 `LD_LIBRARY_PATH`。路径不写死到特定用户主目录。

运行状态保存在 `VOXORCHESTRA_RUN_DIR`：每个服务一个 PID 文件和日志
文件。停止时删除 PID 文件，日志保留用于诊断。

## 启动顺序

1. 对 `edge_gateway`、`unit_manager`、`session_node`、`asr_node`、
   `llm_node`、`tts_node` 执行精确进程名强制清理。
2. 执行 `check_deployment.sh`，检查六个程序、配置、知识库、模型和动态库。
3. 依次启动 ASR、LLM、TTS、Session、Unit Manager 和 Gateway。
4. 确认六个 PID 均存活。
5. 通过 Gateway 发送 `setup`，加载三个真实模型。
6. `setup` 成功后退出启动脚本，六个服务继续在后台运行。

任一程序启动失败、提前退出或 `setup` 失败时，启动入口保留日志，停止
已经启动的进程，清除 PID 文件，并按六个精确进程名执行最终强制清理。

## 停止顺序

`stop.sh` 读取 PID 文件，并通过 `/proc/<pid>/comm` 核对进程名，避免 PID
复用导致误杀。匹配的进程先接收 `SIGTERM`，最多等待 20 秒；超时后改用
`SIGKILL`。PID 文件缺失或进程已经退出不视为错误，因而停止入口可重复
执行。最后再按六个精确进程名清理残留。

## 测试口径

自动化测试使用临时部署目录和后台子进程，至少覆盖：

- 六进程启动并完成 `setup`；
- `setup` 失败后的完整回滚；
- 停止后无进程和 PID 文件残留；
- 缺少运行库根目录时拒绝启动。

自动化测试只验证脚本契约和进程生命周期。模型实际加载、NPU Runtime、
ALSA 设备和端到端推理仍必须在泰山派 3M 上核验。

本发布候选已在无 Git 元数据的板端部署根目录中完成预检和真实 setup，
六个 PID 的进程名均匹配。随后固定 WAV 请求返回 ack，L2 路由产生 34 token、
599 个 PCM 帧且无队列丢弃，WAV 为 16 kHz/单声道/16-bit；停止入口执行后
六进程与 PID 文件均无残留。该单轮结果不替代 30 轮稳定性统计。

## 模型路径与版本

`config/taishanpi3m/session.json` 使用相对部署根目录的路径：

| 组件 | 配置路径 | 运行版本 |
|---|---|---|
| ASR | `models/sherpa-zipformer-bilingual-zh-en-2023-02-16/` | sherpa-onnx + ONNX Runtime 1.17.1 |
| LLM | `models/Qwen3.5-0.8B_w4a16_g128_rk3576.rkllm` | RKLLM Runtime 1.3.0 / RKNPU 0.9.8 |
| TTS | `models/single_speaker_fast.bin` | SummerTTS vits-based |

路径可由部署环境覆盖，但不得只替换模型而混用不兼容的 Runtime/驱动版本链。
RKLLM 的 `rkllm_init` 回调形态在 SDK 版本间变过（1.2.0 build 2025-04-08 为裸
函数指针，之后为 `RKLLMCallback*`），`backends/rkllm/CMakeLists.txt` 在配置期
试编译探测，两种头文件都能直接构建，不需要人工切换开关。

## LLM 采样与思考段配置

`config/*/session.json::llm` 的全部字段（也可用 `llm_node` 的同名命令行参数
覆盖）。改造前这些取值硬编码在 `backends/rkllm/src/rkllm_llm_backend.cpp` 内，
因此换模型必须改代码；现在换模型只改配置：

| 字段 | 默认值 | 说明 |
|---|---|---|
| `model` | — | `.rkllm` 模型文件路径（必填） |
| `max_new_tokens` / `max_context_len` | 100 / 256 | 单轮新增 token 上限与上下文窗口 |
| `top_k` / `top_p` / `temperature` | 1 / 0.95 / 0.8 | 采样参数；`top_k=1` 为确定性贪心 |
| `repeat_penalty` / `frequency_penalty` / `presence_penalty` | 1.1 / 0 / 0 | 重复与频率惩罚 |
| `skip_special_token` / `ignore_eos_token` | true / false | 跳过特殊 token；忽略 EOS 仅用于对照实验 |
| `enable_thinking` | false | 思考模式开关（Qwen3 系列生效，映射到 `RKLLMInput.enable_thinking`） |
| `reasoning_end_tag` | `</think>` | 思考段过滤标记，见下方契约 |
| `reasoning_max_buffer_bytes` | 262144 | 过滤缓冲上限，超限即原样放行，保证内存有界 |
| `enabled_cpus_num` / `enabled_cpus_mask` | 2 / 5（CPU0+CPU2） | 参与推理的 CPU 核数与掩码 |
| `embed_flash` / `base_domain_id` | true / 0 | 词嵌入取自闪存；基座模型域 id |

表中的"默认值"指代码内置默认；`config/taishanpi3m/session.json` 会显式覆盖其中
若干项——当前模型为 `max_new_tokens=256`、`reasoning_end_tag=""`（关闭过滤）。

**思考段过滤契约**：`reasoning_end_tag` 非空时，最后一次出现该标记之前（含标记）
的内容不下发下游，TTS 因此不朗读思考过程。这个标记必须与模型**实际输出**的
文本一致：

- 模型确实会输出 `<think>…</think>` 时，保持 `</think>`（取最后一次出现，标记前的内容不下发）；
- Qwen3.5-0.8B 在 `enable_thinking=false` 时完全不输出标记，必须把
  `reasoning_end_tag` 设为空串；留成 `</think>` 会把整段回答缓冲到生成结束
  才一次性下发（流式重叠失效，日志会打印一次明确告警）；
- Qwen3.5-0.8B 在 `enable_thinking=true` 时输出的是裸的 `Thinking Process: …`
  文本，**不含任何标记**，因此标记式过滤无法把思考段与正式回答分开——语音
  链路必须保持 `enable_thinking=false`（实测见 `artifacts/llm-integration/`）。

非法采样/运行参数（`max_new_tokens <= 0`、`top_p` 越界、CPU 掩码为 0 等）由
`rkllm_options.hpp::validate()` 在 `llm_node` 启动阶段拦截并返回非零退出码，
不会带进 `rkllm_init`；这层校验不依赖厂商 SDK，默认构建同样生效。

## 诊断脚本边界

`run_real_wav_chain.sh`、`run_mic_chain.sh`、`run_llm_chain.sh`、
`run_inject_test.sh` 和 `run_stability_30.sh` 是阶段性诊断/证据采集入口，
部分保留既有板端目录假设。正式发布只以 `start.sh`、`stop.sh` 和本文的
环境变量为准；不要与诊断脚本并行运行。
