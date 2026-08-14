# 泰山派 3M 全真实部署清单

## 适用范围

本清单对应 板端发布候选的泰山派 3M 部署。板端运行六个进程，接入
sherpa-onnx、RKLLM 和 SummerTTS 真实 Backend；默认 x86 构建继续使用
Fake Backend 做确定性回归，二者不得混为同一运行证据。

## 目标版本链

| 项 | 值 |
|---|---|
| 板卡 | 泰山派 3M-RK3576（4G+64G） |
| 系统 | 官方 Ubuntu 24.04.4 LTS 成品镜像 |
| BSP / 内核 | RK_BUILD_INFO 2026-07-04 / Linux 6.1.99 |
| NPU 驱动 | RKNPU 0.9.8（20240828） |
| LLM Runtime | RKLLM 1.2.0 |
| LLM 模型 | DeepSeek-R1-Distill-Qwen-1.5B W4A16 RK3576 |

## 包内内容

| 项 | 位置 | 说明 |
|---|---|---|
| 源码 | `apps/` `libs/` `backends/` `tests/` | 运行时、Backend 适配和测试 |
| 许可明确的内嵌依赖 | `third_party/nlohmann/json.hpp` | nlohmann-json 3.10.5，MIT 许可头保留 |
| 构建入口 | `deploy/taishanpi3m/build.sh` | default / hardware 两种模式，最多 `-j4` |
| 发布入口 | `check_deployment.sh` `start.sh` `stop.sh` | 预检、六进程启动/setup、幂等停止 |
| 板端配置 | `config/taishanpi3m/session.json` | 真实 Backend、模型相对路径、队列与路由参数 |
| 固定公开输入 | `data/fixtures/` | 非隐私 WAV，仅用于可重复测试 |
| 知识库 | `data/knowledge/knowledge.jsonl` | L0-L3 / BM25 数据 |
| 说明与证据 | `README.md` `docs/` `artifacts/` | 方法、版本、许可、脱敏摘要和哈希 |

## 包外依赖

| 组件 | 提供方式 | 入库策略 |
|---|---|---|
| ZeroMQ / cppzmq / ALSA | Ubuntu 系统包 | 动态链接，不复制库 |
| sherpa-onnx / ONNX Runtime | 板端外部构建目录 | 不提交源码、构建树或 `.so` |
| RKLLM Runtime / 头文件 | Rockchip SDK 目录 | 不提交 SDK 或动态库 |
| SummerTTS / Eigen | 外部源码目录 | 不提交上游源码或构建产物 |
| ASR / LLM / TTS 模型 | 部署环境按哈希提供 | 不提交模型 |

## 明确排除

- `.git/`、`build-*`、CMake 缓存和二进制；
- `.rkllm`、`.onnx`、模型 `.bin`、厂商 SDK、头文件副本和 `.so`；
- 大型原始日志、稳定性原始归档和中间 WAV；
- 现场麦克风录音、板卡地址、开发机个人路径、密码、令牌和私钥。

## 部署顺序

1. 按 `docs/deployment.md` 准备系统包、外部依赖和模型；
2. 用 `build.sh hardware` 原生构建，板端并行度不超过 4；
3. 脚本运行前按精确名称 `pkill -9` 清理六进程；
4. 设置 RKLLM 与 sherpa 根目录，执行 `start.sh`；
5. 检查 `setup.log` 为 ack，完成固定 WAV 或麦克风请求；
6. 执行 `stop.sh`，确认进程和 PID 文件无残留。

板端同步只允许单文件 `scp`，不使用多源 rsync。诊断脚本不作为发布常驻
入口，不能与 `start.sh` 同时运行。
