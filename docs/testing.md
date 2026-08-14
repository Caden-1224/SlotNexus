# 测试矩阵

## 默认 x86 构建

默认配置必须保持 `VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=OFF`。当前 45 个
CTest 覆盖如下：

| 层级 | 覆盖 | 数量/口径 |
|---|---|---|
| 单元与契约 | 信封、解帧、Reactor、队列、RAG、状态机、Fake Backend | CTest 1-19 |
| 集成 | ZeroMQ 三模式、数据面、网关、部署构建/预检/生命周期 | CTest 20-40 |
| E2E 与故障 | Session embedded/net、轮次隔离、会话故障、链路故障 | CTest 41-45 |
| 链接门禁 | 九个应用逐一执行 `ldd` | 不得出现 rkllm/rknn/sherpa/onnx/summer/asound |

```bash
cmake -S . -B build-wsl -DCMAKE_BUILD_TYPE=Debug
cmake --build build-wsl -j4
ctest --test-dir build-wsl --output-on-failure
bash scripts/check_no_hw_deps.sh build-wsl
```

## 无 Git 元数据复现

发布门禁从 `git archive` 解出的目录构建，目录内必须没有 `.git`。由于项目
时间线可能晚于执行机时钟，解包时可用 GNU tar 的 `--touch` 仅规范文件
mtime，不改变内容：

```bash
git archive --format=tar HEAD | tar --touch -xf - -C <clean-dir>
test ! -e <clean-dir>/.git
cmake -S <clean-dir> -B <clean-dir>/build-clean -DCMAKE_BUILD_TYPE=Release
cmake --build <clean-dir>/build-clean -j4
ctest --test-dir <clean-dir>/build-clean --output-on-failure
bash <clean-dir>/scripts/check_no_hw_deps.sh <clean-dir>/build-clean
```

本次候选结果为 45/45，默认硬件开关为 OFF，九个应用均未链接厂商 SDK
或 ALSA。

## 泰山派硬件构建与部署

| 门禁 | 自动化 | 真机 |
|---|---|---|
| 构建入口参数与缺失依赖 | `taishanpi3m_build_*` | 板端原生构建，最多 `-j4` |
| 部署预检 | `taishanpi3m_deploy_*` | 六个程序、配置、三类模型和动态库 |
| 启停与回滚 | `taishanpi3m_*start/stop*` | setup、PID 身份、停止后零残留 |
| Backend 契约 | x86 Fake/协议回归 | sherpa-onnx、RKLLM、SummerTTS、ALSA 各自板端核验 |
| 全真实 E2E | 不以 Fake 结果代替 | 固定 WAV、现场麦克风、故障注入、30 轮稳定性 |

脚本运行前清理 `edge_gateway`、`unit_manager`、`session_node`、`asr_node`、
`llm_node`、`tts_node` 六个精确进程名。板端同步只逐文件使用 `scp`，不使用
多源 rsync。

## 发布判定

自动化测试通过不能替代真机门禁。发布候选要求三种真实模型、真实
音频输入/输出路径、故障恢复和稳定性证据同时成立；Fake 仅证明协议与编排
的确定性行为。
