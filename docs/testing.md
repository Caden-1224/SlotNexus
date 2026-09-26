# 测试矩阵

## 默认 x86 构建

默认配置必须保持 `SLOTNEXUS_ENABLE_HARDWARE_BACKENDS=OFF`。当前 46 个
CTest 覆盖如下：

| 层级 | 覆盖 | 数量/口径 |
|---|---|---|
| 单元与契约 | 信封、解帧、Reactor、队列、RAG、状态机、Fake Backend、RKLLM 纯逻辑 | CTest 1-20 |
| 集成 | ZeroMQ 三模式、数据面、网关、部署构建/预检/生命周期 | CTest 21-41 |
| E2E 与故障 | Session embedded/net、轮次隔离、会话故障、链路故障 | CTest 42-46 |
| 链接门禁 | 九个应用逐一执行 `ldd` | 不得出现 rkllm/rknn/sherpa/onnx/summer/asound |

```bash
cmake -S . -B build-wsl -DCMAKE_BUILD_TYPE=Debug
cmake --build build-wsl -j4
ctest --test-dir build-wsl --output-on-failure
bash scripts/check_no_hw_deps.sh build-wsl
```

## 无 Git 元数据复现

发布门禁从 `git archive` 解出的目录构建，目录内必须没有 `.git`。由于项目
时间线可能晚于执行机时钟，解包时可用 GNU tar 的 `--touch` 仅规范文件
mtime，不改变内容：

```bash
git archive --format=tar HEAD | tar --touch -xf - -C <clean-dir>
test ! -e <clean-dir>/.git
cmake -S <clean-dir> -B <clean-dir>/build-clean -DCMAKE_BUILD_TYPE=Release
cmake --build <clean-dir>/build-clean -j4
ctest --test-dir <clean-dir>/build-clean --output-on-failure
bash <clean-dir>/scripts/check_no_hw_deps.sh <clean-dir>/build-clean
```

当前默认构建结果为 46/46，默认硬件开关为 OFF，九个应用均未链接厂商 SDK
或 ALSA。

## 泰山派硬件构建与部署

| 门禁 | 自动化 | 真机 |
|---|---|---|
| 构建入口参数与缺失依赖 | `taishanpi3m_build_*` | 板端原生构建，最多 `-j4` |
| 部署预检 | `taishanpi3m_deploy_*` | 六个程序、配置、三类模型和动态库 |
| 启停与回滚 | `taishanpi3m_*start/stop*` | setup、PID 身份、停止后零残留 |
| Backend 契约 | x86 Fake/协议回归、`rkllm_reasoning_filter_test` 纯逻辑 | sherpa-onnx、RKLLM、MeloTTS、ALSA 各自板端核验 |
| 全真实 E2E | 不以 Fake 结果代替 | 固定 WAV、现场麦克风、故障注入、30 轮稳定性 |

`rkllm_llm_test`（硬件后端构建）的模型与取样参数全部经环境变量注入，因此
同一块板可以在不改仓库的情况下做模型对照：

| 环境变量 | 默认 | 说明 |
|---|---|---|
| `SLOTNEXUS_RKLLM_MODEL` | 空（跳过整组） | `.rkllm` 模型路径；由 CMake 缓存变量写入测试环境 |
| `SLOTNEXUS_RKLLM_MAX_NEW_TOKENS` / `_MAX_CONTEXT_LEN` | 100 / 256 | 单轮 token 上限与上下文窗口 |
| `SLOTNEXUS_RKLLM_TOP_K` / `_TOP_P` / `_TEMPERATURE` / `_REPEAT_PENALTY` | 1 / 0.95 / 0.8 / 1.1 | 采样参数 |
| `SLOTNEXUS_RKLLM_ENABLE_THINKING` | 0 | 思考模式开关（Qwen3 系列） |
| `SLOTNEXUS_RKLLM_REASONING_END_TAG` | `off`（由 CMake 缓存变量写入测试环境） | 思考段过滤标记；取值 `-` 或 `off` 表示关闭过滤（当前 Qwen3.5 不输出思考段标记，必须关闭；留成 `</think>` 会让"生成中取消"用例的前提失效） |

Qwen3.5-0.8B 的对照方法与实测结果见
`artifacts/llm-integration/llm-node-rkllm-qwen35.md`。

`melotts_tts_test`（硬件后端构建）的五个资源路径同样经环境变量注入，
任一缺失即整组跳过（返回 0），因此没有 MeloTTS 依赖的机器不会挂死 ctest：

| 环境变量 | 默认 | 说明 |
|---|---|---|
| `SLOTNEXUS_MELOTTS_ENCODER` / `_DECODER` | 空（跳过整组） | `encoder-zh.onnx` / `decoder-zh.rknn` |
| `SLOTNEXUS_MELOTTS_LEXICON` / `_TOKENS` | 空（跳过整组） | `lexicon.txt` / `tokens.txt` |
| `SLOTNEXUS_MELOTTS_G` | 空（跳过整组） | 说话人 g 向量（恰好 1024 字节） |

`build.sh hardware` 会把这些缓存变量一并传给 ctest；`melotts_tts_test`
在板端实测 13.0 s 通过（含一次完整合成、取消语义与会话重置）。

`sherpa_asr_test` 的固定文本基线与 **ASR 运行时版本链** 绑定：同一份 int8
模型在旧的板端源码编译链（ONNX Runtime 1.17.1）与当前的 v1.13.8 官方
预编译链（ONNX Runtime 1.28.2、官方 `tokens.txt`）上，`test_wavs/0.wav`
的最终文本不同。换 ASR 运行时或 `tokens.txt` 后必须重新核验并同步更新
`tests/unit/sherpa_asr_test.cpp` 中的期望值，证据见
`artifacts/environment-preflight/board-runtime-restore.md`。

脚本运行前清理 `edge_gateway`、`unit_manager`、`session_node`、`asr_node`、
`llm_node`、`tts_node` 六个精确进程名。板端同步只逐文件使用 `scp`，不使用
多源 rsync。

## 发布判定

自动化测试通过不能替代真机门禁。发布候选要求三种真实模型、真实
音频输入/输出路径、故障恢复和稳定性证据同时成立；Fake 仅证明协议与编排
的确定性行为。
