# 第三方组件与许可记录

VoxOrchestra 自有代码使用根目录 MIT License。仓库只内嵌一份
nlohmann-json 单头文件；其余第三方组件由系统或部署环境提供。

| 组件 | 版本/来源 | 许可证 | 用途 | 仓库策略 |
|---|---|---|---|---|
| nlohmann-json | 3.10.5，`third_party/nlohmann/json.hpp` | MIT | JSON 信封 | 内嵌；保留版权和许可头 |
| libzmq | Ubuntu 22.04: 4.3.4；板端 Ubuntu 24.04: 4.3.5 | MPL-2.0 | REQ/REP、PUB/SUB、PUSH/PULL | 系统包动态链接，不内嵌 |
| cppzmq | 板端 4.10.0 | MIT | ZeroMQ C++ 头封装 | 系统包，不内嵌 |
| sherpa-onnx | 外部源码构建 | Apache-2.0 | 流式 ASR | 源码、构建树和 `.so` 不入库 |
| ONNX Runtime | 1.17.1 aarch64 | MIT | sherpa-onnx 推理 | 外部动态库，不入库 |
| RKLLM Runtime/API | airockchip rknn-llm 1.2.0（build 2025-04-08）与 1.3.0；`rkllm_init` 回调形态不同，构建期试编译探测 | Rockchip 分发包所附许可 | RK3576 LLM 推理 | SDK、头文件和 `.so` 不入库 |
| SummerTTS | vits-based，作者仓库 2024-12-14 声明 | MIT | TTS 推理 | 外部源码构建，不复制上游源码 |
| Eigen | 3.4.0，随 SummerTTS 外部目录提供 | MPL-2.0 及文件级兼容许可 | SummerTTS 数值计算 | 外部依赖，不入库 |
| ALSA libasound | Ubuntu 系统包 | LGPL-2.1-or-later | 麦克风与播放 | 系统动态库，不内嵌 |
| Qwen3.5-0.8B RKLLM 模型 | rknn-llm model zoo 的 RK3576 W4A16 G128 转换产物 | 上游模型条款及转换产物分发边界 | LLM 模型（对照） | 再分发权未确认，不入库 |
| sherpa Zipformer 模型 | sherpa-onnx 官方模型 | 上游模型随附条款 | ASR 模型 | 大文件，不入库 |
| single_speaker_fast.bin | SummerTTS 作者资源 | 再分发边界未单独确认 | TTS 模型 | 大文件，不入库 |

## 参考工程边界

`开源参考/Edge-LLM-Infra` 与 `开源参考/LLM_Voice_Flow` 的自有集成代码
没有明确许可证，因此仅研究架构和接口，不复制源码。详细台账见
`artifacts/environment-preflight/source-reuse-ledger.md`。

## 发布规则

- 模型、厂商 SDK、动态库、镜像和外部源码不进入发布仓库；
- 动态链接组件仍须在部署环境保留其许可证和 NOTICE；
- 新增或升级第三方组件时同步更新本文件、版本链和源码复用台账；
