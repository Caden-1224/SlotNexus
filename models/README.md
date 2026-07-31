# 模型获取说明

本目录不存放任何模型文件（`.rkllm` / `.onnx` / `.bin` 等），只记录获取来源与版本链。

| 模型 | 用途 | 版本链（镜像/驱动/Runtime/模型） | 来源 | 校验 |
|---|---|---|---|---|
| `single_speaker_fast.bin` | SummerTTS vits 单说话人合成（TTS 后端，板端） | Ubuntu 24.04 / 无 NPU（纯 Eigen 推理） | 作者仓库 `tts/`（获取方式见其 README，不记录下载链接） | 80050316 B，SHA256 `87b77481…4951c4`（与 `artifacts/upstream-baseline/` 一致） |
| `sherpa-zipformer-bilingual-zh-en-2023-02-16/`（int8） | sherpa-onnx streaming zipformer 中英双语 ASR（ASR 后端，板端） | Ubuntu 24.04 / onnxruntime 1.17.1（aarch64）/ sherpa-onnx 板端源码编译 | sherpa-onnx 官方 + 作者仓库 `voice/models/`（板端 `~/workspace/upstream_rkllm/models/`） | 见 `artifacts/upstream-baseline/`：encoder int8 42980793 B / decoder 3486740 B / joiner 3228485 B / tokens.txt 62574 B / bpe.model 244836 B，SHA256 已核 |
| `DeepSeek-R1-Distill-Qwen-1.5B_w4a16_RK3576.rkllm` | RKLLM 大模型生成（LLM 后端，板端） | Ubuntu 24.04 / RKNPU driver（内核 6.1.99）/ librkllmrt.so v1.2.0 / 模型 W4A16（RK3576） | 开发机模型源（作者提供） | 1394162444 B，SHA256 `5f163f25…f6c5437`（与 `artifacts/upstream-baseline/` 一致） |

规则：

- 每个模型必须与其 Runtime、驱动、镜像版本配套记录，见 `artifacts/environment-preflight/versions.txt`。
- 模型体积大且许可证不明确，一律不进入公开仓库。
