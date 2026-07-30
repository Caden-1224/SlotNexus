# SherpaAsrBackend 板端核验（全链路识别对照）

> 日期：2026-07-30（sherpa-onnx ASR 后端接入的板端验收；
> 门禁基线见 `artifacts/upstream-baseline/upstream-baseline.md` 第二段）

## 环境变更

- sherpa-onnx 真实后端 setup 含模型加载（onnxruntime 初始化 + 3 个 int8
  onnx 加载，实测 ~9.3 s），超过控制面两处硬编码 3 s RPC 超时
  （`edge_gateway::kForwardDeadline` / `unit_manager::kNodeRpcDeadline`）：
  改为构造注入 + CLI 参数（`--forward-timeout-ms` / `--node-rpc-timeout-ms`，
  默认 3000 ms 不变，x86 Mock 回归不受影响）。板端核验用 15000 ms。
- 调试工具 `scripts/gateway_probe.py` 客户端 socket 超时原为 3 s（连接与
  读取共用），同样被 9.3 s 加载击穿：改为可选第三参数，默认 20 s。
- 模型目录（int8 子集：encoder/decoder/joiner int8 + tokens.txt + bpe.model
  + test_wavs）按 tts 模式 staged 到仓库 `models/`（gitignored，不入库），
  复制后 sha256sum 与门禁基线逐一一致（encoder db6f5155… 等）。
- 构建：`-DVOXORCHESTRA_SHERTA_ROOT=/home/lckfb/workspace/upstream_rkllm/
  sherpa-onnx`（含 `sherpa-onnx/c-api/c-api.h` 与 `build/lib/
  libsherpa-onnx-c-api.so`；onnxruntime 由 sherpa 库自身 RUNPATH 解析，
  无需 LD_LIBRARY_PATH），`-DVOXORCHESTRA_ASR_MODEL=...` 注入单测。

## 单元测试（sherpa_asr_test）

模型目录经 `VOXORCHESTRA_ASR_MODEL` 注入。固定 WAV `test_wavs/0.wav`
（10.05 s，中英混合日期句），4 线程：

| 断言 | 结果 |
|---|---|
| kPartial 渐进（文本随 0.2 s 块更新），末事件 kFinal | 通过 |
| 最终文本 = 门禁基线 `昨天是 MONDAY TODAYS TOMORROW是星` | 通过 |
| RTF（不含模型加载） | 0.136（识别 1.37 s / 音频 10.05 s）|
| 取消：cancel 后 feed_audio 无任何事件 | 通过 |
| 会话重置：新 set_event_callback 清除取消状态 | 通过 |

## 全链路（gateway → manager → asr_node）

edge_gateway(9100, `--forward-timeout-ms 15000`) → unit_manager
(`--node tcp://127.0.0.1:19201 --node-rpc-timeout-ms 15000`) →
asr_node(19201，`--config config/taishanpi3m/session.json`，asr 段
backend=sherpa_onnx / model=models/sherpa-zipformer-bilingual-zh-en-2023-02-16
/ num_threads=4 全配置驱动，无 CLI 覆盖)。

| 项 | 值 |
|---|---|
| setup | 分配 work_id `w-0`；**9.8 s**（含 onnxruntime 初始化 + 模型加载 ~9.3 s，热启动后每次 setup 重复加载）|
| 固定 WAV 推理响应 | payload `{"text":"昨天是 MONDAY TODAYS TOMORROW是星"}` |
| 与 smoke 参考对照 | **逐字一致**（`sherpa_asr_smoke <模型目录> <wav> 4`：text 相同，RTF 1.075 含加载）|
| 节点级耗时 | 1.59 s（不含加载 ≈ RTF 0.16）|
| taskinfo | state=ready，inference_count=1 |
| asr_node 峰值 RSS | VmHWM 177820 kB ≈ **173.7 MB**（SummerTTS 407.9 MB 的 43%；与 LLM ~1.3 GiB 同驻余量充足）|

smoke 参考：`~/workspace/upstream_rkllm/smoke/sherpa_asr_smoke`（门禁产物，
同一 int8 模型 + test_wavs/0.wav + 4 线程复现基线）。

## 结论

SherpaAsrBackend 与 FakeAsrBackend 协议等价（kPartial 渐进 + 末事件 kFinal，
无 kDone，会话边界由 is_last 控制），板端 asr_node 进程内 WAV → 16 kHz
mono S16 PCM 帧 → 最终识别文本，与上游参考**逐字一致**；识别 RTF ~0.16
（不含加载）远低于 1，峰值 RSS ~174 MB 轻量。setup 含 ~9.3 s 模型加载，
控制面 RPC 超时已参数化（默认 3000 ms 不变）。优雅退出：三进程 SIGTERM
全部退出。可进入会话流水线联调（RkllmBackend / ALSA 录放后全真实链路）。
