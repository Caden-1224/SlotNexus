# 第三方源码与 SDK 边界

本目录只允许存放经许可核验、适合再分发的小型依赖。目前唯一内嵌内容是
`nlohmann/json.hpp`（nlohmann-json 3.10.5，MIT，许可头保留）。

以下依赖由部署环境提供，不复制到本目录：

| 组件 | 构建参数或发现方式 | 版本/许可 |
|---|---|---|
| ZeroMQ / cppzmq | Ubuntu `libzmq3-dev` / `cppzmq-dev` | MPL-2.0 / MIT |
| ALSA | CMake `find_package(ALSA)` | LGPL-2.1-or-later，系统动态库 |
| sherpa-onnx | `SLOTNEXUS_SHERTA_ROOT` | Apache-2.0 |
| RKLLM Runtime/API | `SLOTNEXUS_RKLLM_ROOT` | 1.3.0，Rockchip 分发包许可 |
| MeloTTS / ONNX Runtime / RKNN | `SLOTNEXUS_MELOTTS_ROOT` + 模型路径 | 上游 MeloTTS 许可 / MIT / Rockchip 分发包许可 |

模型获取与哈希见 `modules/voice/models/README.md`，完整登记见根目录
`THIRD_PARTY_NOTICES.md`。旧 SummerTTS / Eigen 不再参与当前构建。
