# RkllmBackend 模型解耦与 Qwen3.5-0.8B 板端对照

> 日期：2026-09-26（LLM 后端参数解耦的板端验收）
> 基线对照见同目录 `llm-node-rkllm.md`（上一代 1.5B LLM 基线 / RKLLM 1.2.0；该文是历史记录，保留当时的模型名与哈希）

## 变更范围

改造前，采样参数、CPU 绑核、`embed_flash` 以及针对单一模型的 `</think>`
剥离全部硬编码在 `backends/rkllm/src/rkllm_llm_backend.cpp` 内，
换模型必须改代码。本次把这些取值外提为 `RkllmOptions`
（`backends/rkllm/include/voxorchestra/backend/rkllm/rkllm_options.hpp`），由
`config/*/session.json::llm` 与 `llm_node` 命令行参数注入：

| 文件 | 作用 |
|---|---|
| `backends/rkllm/include/.../rkllm_options.hpp` | 全部可配置项 + `validate()`，不含厂商类型 |
| `backends/rkllm/include/.../rkllm_reasoning_filter.hpp` | 思考段过滤（跨 token 闭合、重复标记取最后一次、超限放行、未闭合回退） |
| `backends/rkllm/src/rkllm_llm_backend.cpp` | 用注入选项构造 `RKLLMParam` / `RKLLMInput`，泵队列改用过滤器 |
| `apps/llm_node/main.cpp` | 解析 `session.json::llm.*` 与新增命令行参数；启动期 `validate()` 快速失败 |
| `tests/unit/rkllm_reasoning_filter_test.cpp` | 纯逻辑单测（默认构建即运行，不需要 NPU） |
| `config/taishanpi3m/session-qwen35.json` | Qwen3.5-0.8B 对照配置（与基线仅 LLM 段不同） |

顺带发现并处理了 SDK 接口漂移：`rkllm_init` 的第三个参数在 RKLLM 1.2.0
（build 2025-04-08）是裸函数指针，之后改为 `RKLLMCallback*`（回调返回
`int`）。`backends/rkllm/CMakeLists.txt` 在配置期用试编译探测两种形态，
两种头文件都能直接构建，不需要人工开关。

> **证据代际**：本文的命令与环境变量按当前项目名书写；`artifacts/` 下更早的
> 证据保留当时的项目名与路径，属于历史记录，未做改写。

## 板端环境

| 项 | 值 |
|---|---|
| 板卡 | 泰山派 3M-RK3576（4 GB，8 核），内核 6.1.99 |
| 空闲内存 | total 3896 MB / available 3352 MB（测试前） |
| RKLLM Runtime | 1.3.0（`rkllm-runtime version: 1.3.0`，rknpu driver 0.9.8） |
| RKLLM 头文件 | 板端既有 SDK 目录 `~/workspace/nexweave-board/deps/include/rkllm.h`（`RKLLMCallback*` 形态） |
| 库 | `~/workspace/nexweave-board/deps/lib/librkllmrt.so`（`LD_LIBRARY_PATH` + RUNPATH） |
| 模型 | `~/rkllm_demo/qwen3.5-0.8b/Qwen3.5-0.8B_w4a16_g128_rk3576.rkllm` |

凭据核对：模型 1125915612 B，SHA256
`e3ec6b45e9e888063a6ee56d79ba4474c4416ec8db17f0ca7026a624e1c940d6`，
开发机与板端两处实测一致。模型自带参数：`max_context_limit 4096`、
`npu_core_num 2`、`model_dtype W4A16_G128`、`Using mrope`。

本次只在板端编译并运行 LLM 单后端用例：源码（不含模型、SDK、构建目录）
放在板端临时目录 `~/workspace/voxorchestra-runtime`。板上当前没有本项目的
六进程部署，基线用的 sherpa-onnx / SummerTTS SDK 目录也已不存在，因此
**没有**跑全链路：

```bash
g++ -O2 -std=c++17 -pthread -DVOXORCHESTRA_RKLLM_INIT_CALLBACK_STRUCT=1 \
  -Ibackends/include -Ibackends/rkllm/include -I"$DEPS/include" \
  tests/unit/rkllm_llm_test.cpp backends/rkllm/src/rkllm_llm_backend.cpp \
  -L"$DEPS/lib" -lrkllmrt -Wl,-rpath,"$DEPS/lib" -o /tmp/voxorchestra_rkllm_test
```

## 回调形态探针（先确认运行时行为）

用最小探针（`rkllm_init` + `rkllm_run_async`，逐回调打印状态/文本/时刻）确认
Qwen3.5-0.8B 在该 Runtime 上是**逐 token 流式**回调：`enable_thinking=0` 时
40 条 `NORMAL` + 1 条 `FINISH`，首 token 3.03 s，末 token 9.08 s。

`enable_thinking=1` 时同一探针得到 195 条 `NORMAL`，内容是**英文思考过程**
（`Thinking Process: 1. **Analyze the Request:** …`），并且**全程不含任何
`<think>` / `</think>` 标记**（196 条回调里无一条包含 `<` 或 `>`）。结论：
标记式过滤对 Qwen3.5 的思考段无效，语音链路必须保持
`enable_thinking=false`。

## 单元测试（rkllm_llm_test，Qwen3.5-0.8B）

配置：`max_new_tokens=120 / max_context_len=256 / top_k=1 / top_p=0.95 /
temperature=0.8 / enable_thinking=0 / reasoning_end_tag=""`。

| 断言 | 结果 |
|---|---|
| kToken 流（全部非空）+ 末事件 kDone，kDone.text = token 拼接 | 通过 |
| 流式粒度 | 39 个 kToken（逐 token 下发，非整段落地） |
| TTFT | 2.95–3.02 s（多次运行） |
| 解码速率 | 6.5–7.5 tok/s（decode 窗口内，多次运行） |
| 输出文本 | `你好！我是 Qwen3.5，由通义实验室自主研发的超大规模语言模型，能处理中文、英文及多语言任务，并具备代码生成与逻辑推理能力。`（多次运行逐字一致，top_k=1 贪心） |
| 取消：cancel 后 generate 无任何事件 | 通过 |
| 生成中取消：cancel 后无新事件、无 kDone（旧 token 过滤） | 通过 |
| 会话重置：新 set_event_callback 清除取消状态 | 通过（输出逐字一致） |

同一模型对照上一代基线（1.5B，RKLLM 1.2.0，回答约
30–60 s）缩短到约 8.3–8.5 s 整轮、TTFT 约 3.0 s，量级差异来自模型规模
（0.8B vs 1.5B）与 Runtime 版本，不能只归因于其中一个。

## 错误配置对照（`reasoning_end_tag` 与模型不匹配）

把 `reasoning_end_tag` 留成 `</think>`（模型实际不输出该标记）后，过滤器会
把整段回答缓冲到生成结束才回退放行：

| 观察项 | 结果 |
|---|---|
| 后端告警 | 打印一次：`[rkllm] 本轮生成结束仍未出现思考段标记 "</think>"，被缓冲的回答延迟到生成结束才下发；若该模型不输出思考段，请把 reasoning_end_tag 设为空` |
| 流式粒度 | 退化为 1 个 kToken（TTFT 8500.5 ms = 总耗时 8.50 s） |
| 输出内容 | 不丢失，与正确配置逐字一致 |
| 「生成中取消」用例 | 失败（`!has_done`）：整段回答在结束时一次性落地，用例的前提「首个事件早于生成结束」不再成立 |

即：这是配置错误导致的性能退化，不是数据丢失，也不是后端缺陷；告警与用例
失败都是该退化模式的可见信号。

## 未测量 / 边界

- 六进程全链路（gateway → manager → session → llm）未跑：板上没有本次改动后
  的 VoxOrchestra 部署，基线 SDK 目录缺失，且 ASR/TTS 仍是旧模型链；本次只
  证明 LLM 后端在 Qwen3.5-0.8B 上可用。
- 未测 NPU/CPU 争用：Qwen3.5-0.8B 单后端独占时可用，与 ASR/TTS 并发时的
  首 token 延迟、吞吐与 RSS 均无数据。
- 未测长时运行（温度/频率劣化）、未测 30 轮。
- TTS 朗读链路未验证，Qwen3.5 输出中的英文与数字标点的分词行为未评估。
- 待跟进风险（本次未定位到后端内部）：400 token 的探针在 30 s 等待上限内没有
  等到 `FINISH`，超时后直接 `rkllm_destroy`，此时厂商线程仍投递回调，进程以
  exit 139（段错误）结束。后端的析构路径同样只在 `running` 为真时
  `rkllm_abort`（尽力而为）再 `rkllm_destroy`，没有等待在途回调的机制；单元
  测试的四组用例未复现，但"在途回调期间销毁句柄"需要一次专门的析构竞态验证。

## 结论

LLM 后端已与具体模型解耦：换模型只改 `session.json::llm`（或命令行参数），
代码内不再有单一模型的采样参数或思考段假设；Qwen3.5-0.8B 在板端完成真实
流式生成、取消与会话重置验证，输出确定且可读，TTFT 约 3.0 s。语音链路使用
Qwen3.5 时必须 `enable_thinking=false` 且清空 `reasoning_end_tag`。
