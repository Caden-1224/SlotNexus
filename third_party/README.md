# 第三方源码 / SDK 获取说明

本目录不存放第三方源码与厂商 SDK（sherpa-onnx、RKLLM Runtime、SummerTTS、ALSA 头文件等），只记录获取方式。对应许可记录见根目录 `THIRD_PARTY_NOTICES.md`。

| 组件 | 用途 | 获取来源 | 版本链说明 | 许可证 |
|---|---|---|---|---|
| SummerTTS 源码（`tts/`） | TTS 后端推理（`backends/summer_tts`） | 作者仓库 `tts/`，板端 `~/upstream_tts/tts/`（含 src/ include/ eigen-3.4.0/） | 构建时经 `-DVOXORCHESTRA_SUMMERTTS_ROOT=<源码根>` 指向板端目录；依赖 Eigen 3.4.0 自带，无外部 NN 运行时 | MIT |
