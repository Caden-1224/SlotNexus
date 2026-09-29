# 泰山派 3M 全真实部署清单

## 适用范围

本清单对应 板端发布候选的泰山派 3M 部署。板端运行六个进程，接入
sherpa-onnx、RKLLM 和 MeloTTS 真实 Backend；默认 x86 构建继续使用
Fake Backend 做确定性回归，二者不得混为同一运行证据。

## 目标版本链

| 项 | 值 |
|---|---|
| 板卡 | 泰山派 3M-RK3576（4G+64G） |
| 系统 | 官方 Ubuntu 24.04.4 LTS 成品镜像 |
| BSP / 内核 | RK_BUILD_INFO 2026-07-04 / Linux 6.1.99 |
| NPU 驱动 | RKNPU 0.9.8（20240828） |
| LLM Runtime | RKLLM 1.3.0；`rkllm_init` 回调形态在 SDK 版本间变过，构建期试编译探测 |
| LLM 模型 | Qwen3.5-0.8B W4A16 G128 RK3576 |
| TTS Runtime | MeloTTS（ONNX Runtime CPU 编码器 + RKNN NPU 解码器） |

## 包内内容

| 项 | 位置 | 说明 |
|---|---|---|
| 源码 | `middleware/` `modules/voice/` `tests/core/` | 通用中间件、语音模块与测试 |
| 许可明确的内嵌依赖 | `third_party/nlohmann/` | nlohmann-json 3.10.5 完整 multi-header 头集合，MIT 许可头保留 |
| 构建入口 | `modules/voice/deploy/taishanpi3m/build.sh` | default / hardware 两种模式，最多 `-j4` |
| 发布入口 | `check_deployment.sh` `start.sh` `stop.sh` | 预检、六进程启动/setup、幂等停止 |
| 测量与回归入口 | `run.sh` + `common.sh` | 基线、固定 WAV、麦克风、稳定性、注入等场景；六进程启动参数与优雅退出只有一处 |
| 板端配置 | `modules/voice/config/taishanpi3m/session.json` | 真实 Backend、模型相对路径、LLM 采样与思考段过滤、队列与路由参数 |
| 固定公开输入 | `SLOTNEXUS_VOICE_FIXTURE_DIR` 指向的板端本地目录 | 真实 WAV 由部署路径注入，仓库默认测试生成最小夹具 |
| 知识库 | `modules/voice/examples/knowledge.jsonl`（最小示例） | 真实知识库由部署路径注入 |
| 说明 | `README.md` | 方法、版本与哈希 |

## 包外依赖

| 组件 | 提供方式 | 入库策略 |
|---|---|---|
| ZeroMQ / cppzmq / ALSA | Ubuntu 系统包 | 动态链接，不复制库 |
| sherpa-onnx / ONNX Runtime | 板端外部构建目录 | 不提交源码、构建树或 `.so` |
| RKLLM Runtime / 头文件 | Rockchip SDK 目录 | 不提交 SDK 或动态库 |
| MeloTTS / ONNX Runtime / RKNN | 外部依赖根 + 上游模型 | 不提交依赖根、模型或构建产物 |
| ASR / LLM / TTS 模型 | 部署环境按哈希提供；ASR 默认使用 `asr.model_precision=fp32` 对应的 `.onnx` 三元组，`int8` 可选 | 不提交模型 |

## 明确排除

- `.git/`、`build-*`、CMake 缓存和二进制；
- `.rkllm`、`.onnx`、模型 `.bin`、厂商 SDK、头文件副本和 `.so`；
- 大型原始日志和中间 WAV；
- 现场麦克风录音、板卡地址、开发机个人路径、密码、令牌和私钥。

## 部署顺序

1. 按部署环境准备系统包、外部依赖和模型；
2. 用 `build.sh hardware` 原生构建，板端并行度不超过 4；
3. 脚本运行前由 `stop.sh --force` 清理六进程；
4. 设置 RKLLM 与 sherpa 根目录，执行 `start.sh`；
5. 检查 `setup.log` 为 ack，完成固定 WAV 或麦克风请求；
6. 执行 `stop.sh`，确认进程和 PID 文件无残留。

板端同步只允许单文件 `scp`，不使用多源 rsync。诊断脚本不作为发布常驻
入口，不能与 `start.sh` 同时运行。
