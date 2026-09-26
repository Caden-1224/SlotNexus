# 当前链路 30 轮全真实稳定性（Qwen3.5-0.8B + MeloTTS）

> 日期：2026-09-26（板端实测）
> 入口：`deploy/taishanpi3m/run_stability_30.sh 30`
> 版本链：`artifacts/environment-preflight/versions.txt` 第二节
> 依赖恢复：`artifacts/environment-preflight/board-runtime-restore.md`
> 单轮全链路：`artifacts/llm-integration/llm-node-rkllm-qwen35.md`
> TTS 侧：`artifacts/tts-integration/tts-node-melotts.md`

## 口径

- 板卡：泰山派 3M，RK3576，4 GB；官方 Ubuntu 24.04.4，内核 6.1.99；
- ASR：sherpa-onnx v1.13.8 官方 aarch64 预编译件（ONNX Runtime 1.28.2），
  int8 streaming zipformer small bilingual zh-en；
- LLM：RKLLM 1.3.0 + Qwen3.5-0.8B W4A16 G128（`max_new_tokens=256`、
  `max_context_len=256`、`enabled_cpus=[0,2]`、`enable_thinking=false`、
  `reasoning_end_tag=""`）；
- TTS：MeloTTS（ONNX Runtime CPU 编码器 + RKNN NPU 解码器，`speed=0.8`）；
- 输入：仓库固定 `data/fixtures/demo_zh.wav`（16 kHz/单声道/16-bit，3.328 s），
  顺序 30 轮；
- 每轮重建六进程（规避 RKLLM/RKNPU 长时劣化累积）并在轮前 `drop_caches`；
- 统计：`stability-30-qwen35-melotts.csv`（本轮原始汇总，已入库）。

## 结果

| 指标 | 结果 |
|---|---|
| 成功率 | **30/30** |
| 端到端 p50 | **59,419 ms** |
| 端到端 p95 | **60,804 ms** |
| 端到端 min / max | 57,454 / 61,686 ms |
| 端到端 mean / 总体标准差 | 59,549 / 950.8 ms |
| 路由 | 30/30 轮均为 `l2` |
| token 数 | 30/30 轮均为 152 |
| PCM 帧数 | 2,366–2,392（每轮 16 kHz×320 采样帧） |
| ASR RSS 首轮 -> 末轮 | 112,384 -> 112,432 KB（最小 109,212 / 最大 113,240） |
| LLM RSS 首轮 -> 末轮 | 890,964 -> 891,028 KB（最小 890,884 / 最大 891,080） |
| TTS RSS 首轮 -> 末轮 | 220,872 -> 221,700 KB（最小 220,688 / 最大 221,700） |
| 温度 min / max / avg | 49.0 / 51.8 / 50.6 °C |
| 汇总 CSV SHA256 | `c94b740ecff8eaecaf149eeacbec440ab7b39ced3d96101ba14b388aae55e625` |

## 判读

1. **可用性**：30 轮零失败、零队列丢弃（响应中 `dropped_pcm_frames` /
   `dropped_sentences` 均为 0），路由与 token 数逐轮完全一致——同一输入在
   `top_k=1` 贪心采样下输出稳定。
2. **时延收敛**：p50 与 p95 只差 1,385 ms，标准差 950.8 ms（约 1.6%），
   说明端到端耗时以 LLM 解码为主且分布很窄；没有出现上一代 Runtime 那种
   轮次劣化后长尾。
3. **内存无泄漏**：三个节点首末轮 RSS 漂移都在 1 MB 以内
   （asr +48 KB / llm +64 KB / tts +828 KB），最大瞬时值也仅比首轮高约 1 MB。
4. **温度**：全程 49.0–51.8 °C，先升后稳（前 10 轮升到 51.8 °C 后回落并
   在 49.9–51.8 °C 之间波动），没有热失控趋势；该区间高于上一代基线记录的
   43.5–46.2 °C，与 LLM/TTS 同时使用 NPU、负载更高一致。
5. **与上一代基线不可直接比较**：上一代是 1.5B + SummerTTS，p50 56,273 ms /
   p95 61,849 ms；本轮 0.8B + MeloTTS 的 p50 略高（+3.1 s）而 p95 略低
   （-1.0 s），且回答长度不同（上一代发布候选单轮 34 token，本轮 152 token）。
   两组数字只在本口径内成立，**不得混算或相互推断**。

## 未测 / 边界

- 未测并发请求、真实麦克风输入下的 30 轮、更长时间（>30 轮）劣化；
- 未做 CPU 绑核、`max_context_len` / `max_new_tokens`、warmup 的 A/B；
- 温度读数取自 `/sys/class/thermal/thermal_zone0`，未记录频率与功耗；
- 每轮重建六进程规避了长时劣化，因此本结果**不**代表"六进程常驻 30 轮"。
