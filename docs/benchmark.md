# Benchmark 方法与结果

## 口径

本项目只报告泰山派 3M 实测，不引用官方宣传值代替系统结果。稳定性基线
固定以下变量：

- 板卡：泰山派 3M，RK3576，4 GB；官方 Ubuntu 24.04.4，内核 6.1.99；
- NPU：RKNPU 0.9.8（两条链相同）；
- 输入：仓库固定 16 kHz/单声道/16-bit WAV，按顺序执行 30 轮；
- 路径：sherpa-onnx ASR -> Session 内 BM25/L0-L3 -> RKLLM（需要时）
  -> TTS -> WAV；
- 轮次：同一版本链、同一配置、六进程常驻，逐轮保存响应、六进程日志、
  RSS、温度和阶段耗时；
- 统计：端到端耗时报告 p50/p95，不挑选最好轮次；RSS 报首轮到末轮，
  温度报告全程最小值与最大值。

**两条代际不可混算**：上一代链路是 1.5B LLM（RKLLM 1.2.0）+ SummerTTS；
当前链路是 Qwen3.5-0.8B（RKLLM 1.3.0）+ MeloTTS，ASR 运行时也从板端源码
编译链换成 v1.13.8 官方预编译链。两节的数字只在本节口径内成立。

## 30 轮稳定性（上一代 1.5B + SummerTTS 基线）

| 指标 | 结果 |
|---|---|
| 成功率 | 30/30 |
| 端到端 p50 | 56,273 ms |
| 端到端 p95 | 61,849 ms |
| ASR RSS 首轮 -> 末轮 | 136,428 -> 130,096 KB |
| LLM RSS 首轮 -> 末轮 | 1,065,544 -> 1,065,588 KB |
| TTS RSS 首轮 -> 末轮 | 385,544 -> 385,440 KB |
| 温度范围 | 43.461–46.230 °C |
| Fake/Mock 运行标记 | 180 份日志中 0 |

外部原始证据不入仓库。归档文件名为
`full-pipeline-stability-30.tar.gz`，SHA256：
`5cb91dd560072c45066d70da12ba9b26bfa4df13b29edb21c0fa5da2fd880da0`。
汇总 CSV SHA256：
`41c828a3cbc62697a6a73780985a667523fd1b22ecf5256408746a3b23e7858d`。

## 分阶段基线（上一代 LLM 基线）

| 阶段 | 输入 | 实测 |
|---|---|---|
| ASR | 10.05 秒固定 WAV，4 线程 | 不含加载 RTF 约 0.16，峰值 RSS 约 174 MB |
| RKLLM | 固定 prompt，100 token 上限 | TTFT 约 340 ms，约 6.4–6.7 token/s，峰值 RSS 约 1.01 GiB |
| SummerTTS | 固定中文文本 | RTF 约 0.545，峰值 RSS 约 409 MB |
| 发布候选单轮 | 固定 WAV | 57.2 秒，34 token，599 PCM 帧，0 丢弃 |

分阶段数字用于定位，不可与端到端 p50/p95直接相加；模型加载、路由、队列、
句子切分和输出 drain 会改变总耗时。

## 已知性能边界

上述 30 轮基线上，LLM 单次回答约 30–60 秒。RKLLM Runtime 与 RKNPU 0.9.8
存在长时运行劣化，当前证据只支持上述环境、输入和 30 轮口径，不外推到
其他模型、镜像或散热条件。该劣化观测来自上一代 Runtime（1.2.0）；1.3.0
的 30 轮结果见下一节。

## 30 轮稳定性（当前：Qwen3.5-0.8B + MeloTTS）

入口 `deploy/taishanpi3m/run_stability_30.sh 30`，每轮重建六进程并在轮前
`drop_caches`。原始汇总入库：`artifacts/full-chain-stability/
stability-30-qwen35-melotts.csv`，SHA256
`c94b740ecff8eaecaf149eeacbec440ab7b39ced3d96101ba14b388aae55e625`。

| 指标 | 结果 |
|---|---|
| 成功率 | 30/30 |
| 端到端 p50 | 59,419 ms |
| 端到端 p95 | 60,804 ms |
| 端到端 min / max | 57,454 / 61,686 ms |
| 端到端 mean / 标准差 | 59,549 / 950.8 ms |
| 路由 / token 数 | 30/30 轮均为 `l2` / 152 token |
| PCM 帧数 | 2,366–2,392 |
| ASR RSS 首轮 -> 末轮 | 112,384 -> 112,432 KB |
| LLM RSS 首轮 -> 末轮 | 890,964 -> 891,028 KB |
| TTS RSS 首轮 -> 末轮 | 220,872 -> 221,700 KB |
| 温度 min / max / avg | 49.0 / 51.8 / 50.6 °C |

p50 与 p95 仅差 1,385 ms、标准差约 1.6%，三节点 RSS 首末轮漂移均在 1 MB
以内：在当前口径下没有观察到轮次劣化或泄漏。本轮 p50 比上一代 1.5B 基线
高 3.1 s、p95 低 1.0 s，但回答长度不同（34 token vs 152 token），
**两组数字不可混算**。

## 分阶段基线（当前链路）

| 阶段 | 输入 | 实测 |
|---|---|---|
| ASR（sherpa-onnx v1.13.8 + ORT 1.28.2） | 10.05 秒固定 WAV，4 线程 | RTF **0.163**（1.64 s），单轮全链路内峰值 RSS 113,600 kB |
| RKLLM（Qwen3.5-0.8B，1.3.0） | 固定 prompt，`ctx=256` | TTFT **2.84–2.97 s**，decode **5.99–7.24 token/s**，单轮 8.24–9.45 s；峰值 RSS 891,148 kB |
| MeloTTS（ONNX CPU + RKNN） | 固定文本「你好，这是语音合成测试。」 | RTF **0.670**；链路内 6 句 0.488–0.717；峰值 RSS 221,752 kB |
| 发布候选单轮（固定 WAV 全链路） | `demo_zh.wav`，3.328 s | **60.34 s**，152 token，2,379 PCM 帧，0 丢弃，`pcm_queue_peak=32` |

单后端 LLM 的 token 预算在 120 与 256 下都只产生 39 token（贪心输出 39 token
后自然结束），因此 `max_new_tokens` 不是当前瓶颈；链路里 L2 注入知识库上下文
后回答拉长到 152 token，端到端耗时主要由回答长度与 TTS 句数决定。

## 当前链路的已知边界

- 30 轮每轮重建进程，**不**代表"六进程常驻 30 轮"；
- 未测并发请求、真实麦克风下的 30 轮、超过 30 轮的长时劣化；
- 未做 CPU 绑核、`max_context_len`/`max_new_tokens`、warmup 的 A/B；
- MeloTTS 解码器与 RKLLM 共享 NPU，链路内 RTF 与单后端 RTF 不可直接互推；
- 温度取自 thermal_zone0，未记录频率与功耗。
