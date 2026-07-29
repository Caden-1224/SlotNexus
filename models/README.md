# 模型获取说明

本目录不存放任何模型文件（`.rkllm` / `.onnx` / `.bin` 等），只记录获取来源与版本链。

| 模型 | 用途 | 版本链（镜像/驱动/Runtime/模型） | 来源 | 校验 |
|---|---|---|---|---|
| `single_speaker_fast.bin` | SummerTTS vits 单说话人合成（TTS 后端，板端） | Ubuntu 24.04 / 无 NPU（纯 Eigen 推理） | 作者仓库 `tts/`（获取方式见其 README，不记录下载链接） | 80050316 B，SHA256 `87b77481…4951c4`（与 `artifacts/upstream-baseline/` 一致） |

规则：

- 每个模型必须与其 Runtime、驱动、镜像版本配套记录，见 `artifacts/environment-preflight/versions.txt`。
- 模型体积大且许可证不明确，一律不进入公开仓库。
