# MeloTtsBackend 板端核验（替换 SummerTTS）

> 日期：2026-09-26（板端实测，全真实链路重跑的 TTS 侧证据）
> 前置：`artifacts/environment-preflight/board-runtime-restore.md`（依赖恢复过程）
> 历史对照：`artifacts/tts-integration/tts-node-summertts.md`（上一代 SummerTTS 链路）

## 为什么替换

上一代全链路的 TTS 后端是 SummerTTS，模型 `single_speaker_fast.bin` 只在项目
作者的百度网盘中分发，板端与开发机在本轮开始时都已不存在该文件，且仓库纪律
不允许把模型入库。板端与开发机现成可用的是 MeloTTS 的导出件
（`~/rkllm_demo/melotts_probe/encoder-zh.onnx` + `decoder-zh.rknn`），参考工程
nexweave 已用同一套模型跑通真实推理（`run_melotts_hardware_test.sh`）。因此本轮
把 MeloTTS 接入 SlotNexus 的 `ITtsBackend` 契约，SummerTTS 后端保留兼容但默认
不构建。

## 实现

| 文件 | 作用 |
|---|---|
| `backends/melotts/include/slotnexus/backend/melotts/melotts_tts_backend.hpp` | 公共契约：`MeloTtsConfig` + `MeloTtsBackend : ITtsBackend` |
| `backends/melotts/src/melotts_internal.hpp` | 内部结果类型、文本前端、重采样器与两个推理引擎接口 |
| `backends/melotts/src/melotts_text_frontend.cpp` | `lexicon.txt` + `tokens.txt` 自有文本前端（最长匹配、英文回退、按预算切块） |
| `backends/melotts/src/melotts_pcm_converter.cpp` | 44.1 kHz float → 16 kHz S16 的流式线性重采样 |
| `backends/melotts/src/melotts_real_engines.cpp` | ONNX Runtime CPU 编码器 + RKNN NPU 解码器 |
| `backends/melotts/src/melotts_tts_backend.cpp` | 合成状态机、分片解码、重叠丢弃、尾帧补零、统计与日志 |

实现从参考工程 nexweave `core/backend/melotts/` 逐算法移植，只替换结果类型
（`domain::Result/OperationResult` → 本模块最小 `MeloResult/MeloStatus`）与事件
出口（`capability::AudioEventCallback` → `BackendEvent{kPcm…} + kDone`）。
算法本身（文本前端、切片预算与重叠上下文、重采样、尾帧处理）逐行保持一致，
没有引入新的近似。

### 契约一致性（与 `FakeTtsBackend` / `SummerTtsBackend` 等价）

- `synthesize(text)` 产出 `kPcm…`（每帧 ≤ `kFrameSamples=320` 采样，20 ms），
  全部音频产出后 `kDone`；
- `cancel()` 置位后 `synthesize` 不产出任何事件（合成中途取消同样丢弃后续帧）；
- `set_event_callback` 开启新会话并清空取消状态与统计；
- 失败（资源不可读、形状不符、超过预算）不产出任何事件，只在 `stderr` 打印
  结构化原因——协议层没有错误事件，这一点与既有两个后端一致。

### 构建接线

- `SLOTNEXUS_MELOTTS_ROOT` 需提供 `include/{onnxruntime_c_api.h,rknn_api.h}` 与
  `lib/{libonnxruntime.so,librknnrt.so}`；
- `build.sh hardware` 额外注入 `SLOTNEXUS_MELOTTS_{ENCODER,DECODER,LEXICON,TOKENS,G}`
  给 `melotts_tts_test`；
- `tts_node --backend melotts`，模型与文本资源路径全部来自
  `config/taishanpi3m/session.json::tts`，代码内不硬编码；
- `start.sh` / 三个链路诊断脚本的 `LD_LIBRARY_PATH` 追加
  `$SLOTNEXUS_MELOTTS_ROOT/lib`。

## 板端实测

### 单元测试（`melotts_tts_test`，固定文本「你好，这是语音合成测试。」）

| 断言 | 结果 |
|---|---|
| kPcm 帧流（每帧 ≤ 320 采样）+ 末事件 kDone | 通过 |
| 帧总采样 | 40,640（= 2.540 s @16 kHz） |
| RTF | 0.670（合成 1.701 s / 音频 2.540 s） |
| 取消：cancel 后无任何事件 | 通过 |
| 会话重置：新会话不受上次取消影响 | 通过 |
| 用例耗时 | 13.0 s（含模型加载） |

### 全链路（gateway → manager → session → asr/llm/tts）

单轮固定 WAV（`data/fixtures/demo_zh.wav`，3.328 s）跑通后，`tts_node` 日志给出
每句合成指标（`session.json` 的 `speed=0.8`、`length_scale=1/0.8`）：

| 句序 | 文本字节 | 原生样本 | 重采样样本 | PCM 帧 | 音频(s) | 合成(ms) | RTF |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 68 | 252,416 | 91,580 | 287 | 5.72 | 4,106 | 0.717 |
| 2 | 75 | 291,840 | 105,883 | 331 | 6.62 | 3,934 | 0.594 |
| 3 | 131 | 353,280 | 128,174 | 401 | 8.01 | 4,047 | 0.505 |
| 4 | 113 | 374,784 | 135,976 | 425 | 8.50 | 4,144 | 0.488 |
| 5 | 110 | 335,360 | 121,673 | 381 | 7.60 | 3,946 | 0.519 |
| 6 | 148 | 487,936 | 177,029 | 554 | 11.06 | 5,842 | 0.528 |

（链路返回的 `pcm_frames=2379` 是会话侧累计值，包含全部句子。）

| 项 | 值 |
|---|---|
| `tts_node` 峰值 RSS | VmHWM 221,752 kB ≈ 216.6 MB |
| 段内 RTF 区间 | 0.488–0.717 |
| 输出格式 | 16 kHz / 单声道 / 16-bit（`RIFF` 校验通过） |
| 合成期间 NPU/CPU | 编码器 ONNX Runtime CPU（`intra_op_threads=1`）、解码器 RKNN NPU，与 RKLLM 共享 NPU |

## 与 SummerTTS 的对照（不同模型、不同链路，只可定位不可直接比较）

| 项 | SummerTTS（上一代） | MeloTTS（本轮） |
|---|---|---|
| 峰值 RSS | 419,240 kB ≈ 409.4 MB | 221,752 kB ≈ 216.6 MB |
| RTF | 0.545（热启动，单句） | 0.488–0.717（链路内 6 句） |
| 音频与上游参考 | 与 smoke `cmp` 逐字节一致 | 无上游参考产物，只做协议与时长自洽断言 |
| 模型 | `single_speaker_fast.bin`（网盘分发） | `encoder-zh.onnx` + `decoder-zh.rknn`（板端既有） |
| 运行依赖 | Eigen（纯 CPU 模板库） | ONNX Runtime CPU + RKNN NPU |

MeloTTS 峰值 RSS 明显更低（少约 190 MB），但 RTF 略高；两者都小于 1，都满足
实时合成有余量的结论。两者的模型、采样率与文本前端完全不同，
**不得把两份 RTF 放进同一张对照表**，本节只记录量级。

## 已知边界

- 说话人风格由固定 `g-zh_mix_en.bin`（g-zh_mix_en，256 float）决定，不支持换音色；
- 文本前端的数字规范化只做逐位中文读法（`1.5` → `一点五`），不做货币/日期等
  完整 TN；`unknown_units` 会在日志中计数，便于发现词典外片段；
- 解码器走 NPU，与 RKLLM 并发时共享 NPU；本轮全链路的端到端耗时同时包含这段
  争用，单后端 TTS RTF 与链路内 RTF 因此不可直接互推；
- 未测长时运行（30 轮）之外的更长时间劣化；30 轮结果见 `docs/benchmark.md`。
