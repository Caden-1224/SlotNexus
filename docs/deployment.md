# 泰山派 3M 部署与进程生命周期

## 适用范围

本文定义全真实语音链路的板端启动与停止入口。部署包不包含模型、厂商
SDK、动态库、板卡地址或凭据；这些资源由部署环境提供。

## 命令入口

```bash
bash deploy/taishanpi3m/start.sh
bash deploy/taishanpi3m/stop.sh
```

`start.sh` 在后台启动六个服务并完成模型 `setup`。`stop.sh` 停止本次部署
的服务，可重复执行。

## 环境与状态

启动入口使用以下环境变量：

| 变量 | 必需 | 用途 |
|---|---|---|
| `VOXORCHESTRA_RKLLM_ROOT` | 是 | 提供 `aarch64/librkllmrt.so` |
| `VOXORCHESTRA_SHERTA_ROOT` | 是 | 提供 sherpa-onnx 与 ONNX Runtime 动态库 |
| `VOXORCHESTRA_DEPLOY_ROOT` | 否 | 部署根目录，默认由脚本位置推导 |
| `VOXORCHESTRA_BUILD_DIR` | 否 | 硬件构建目录，默认 `build-taishanpi3m-hw` |
| `VOXORCHESTRA_CONFIG` | 否 | 板端配置，默认 `config/taishanpi3m/session.json` |
| `VOXORCHESTRA_RUN_DIR` | 否 | PID 与日志目录，默认 `/tmp/voxorchestra-runtime` |
| `VOXORCHESTRA_SETUP_TIMEOUT_SECONDS` | 否 | `setup` 等待秒数，默认 120 |

运行库搜索路径由 `VOXORCHESTRA_RKLLM_ROOT/aarch64`、
`VOXORCHESTRA_SHERTA_ROOT/build/lib` 和
`VOXORCHESTRA_SHERTA_ROOT/build/_deps/onnxruntime-src/lib` 推导，并保留
调用者已有的 `LD_LIBRARY_PATH`。路径不写死到特定用户主目录。

运行状态保存在 `VOXORCHESTRA_RUN_DIR`：每个服务一个 PID 文件和日志
文件。停止时删除 PID 文件，日志保留用于诊断。

## 启动顺序

1. 对 `edge_gateway`、`unit_manager`、`session_node`、`asr_node`、
   `llm_node`、`tts_node` 执行精确进程名强制清理。
2. 执行 `check_deployment.sh`，检查六个程序、配置、知识库、模型和动态库。
3. 依次启动 ASR、LLM、TTS、Session、Unit Manager 和 Gateway。
4. 确认六个 PID 均存活。
5. 通过 Gateway 发送 `setup`，加载三个真实模型。
6. `setup` 成功后退出启动脚本，六个服务继续在后台运行。

任一程序启动失败、提前退出或 `setup` 失败时，启动入口保留日志，停止
已经启动的进程，清除 PID 文件，并按六个精确进程名执行最终强制清理。

## 停止顺序

`stop.sh` 读取 PID 文件，并通过 `/proc/<pid>/comm` 核对进程名，避免 PID
复用导致误杀。匹配的进程先接收 `SIGTERM`，最多等待 20 秒；超时后改用
`SIGKILL`。PID 文件缺失或进程已经退出不视为错误，因而停止入口可重复
执行。最后再按六个精确进程名清理残留。

## 测试口径

自动化测试使用临时部署目录和后台子进程，至少覆盖：

- 六进程启动并完成 `setup`；
- `setup` 失败后的完整回滚；
- 停止后无进程和 PID 文件残留；
- 缺少运行库根目录时拒绝启动。

自动化测试只验证脚本契约和进程生命周期。模型实际加载、NPU Runtime、
ALSA 设备和端到端推理仍必须在泰山派 3M 上核验。
