# 泰山派 3M 部署清单（WSL Mock 冻结交付）

> 适用范围：Mock 冻结（M1 门禁）阶段的板端部署包——在泰山派 3M（RK3576）
> 上原生构建并运行与 WSL Mock 完全一致的会话编排链。真实硬件后端
> （sherpa-onnx / RKLLM / SummerTTS / ALSA）不在本包内，属板卡阶段逐级验证。

## 目标板卡

| 项 | 值 |
|---|---|
| 板卡 | 泰山派 3M-RK3576 开发板（4G+64G） |
| SoC | Rockchip RK3576，八核，NPU 最高 6 TOPS |
| 系统 | 官方 Ubuntu 24.04 成品镜像（板卡已烧录；刷写工具与流程见官方下载中心） |
| 运行形态 | 三进程常驻：edge_gateway / unit_manager / session_node |

## 包内容清单

| 项 | 位置 | 说明 |
|---|---|---|
| 源码 | `libs/` `apps/` `tests/` `cmake/` `third_party/` | 全部源码随包；`third_party/nlohmann/json.hpp` 为单头文件回退依赖 |
| 构建脚本 | `deploy/taishanpi3m/build.sh` | 板端原生构建 + CTest（aarch64） |
| 运行脚本 | `deploy/taishanpi3m/run_mock_chain.sh` | 三进程启动、冒烟验证、优雅收尾 |
| 板端配置 | `config/taishanpi3m/session.json` | 板端运行参数（路由阈值、队列容量、兜底上限） |
| 知识库 | `data/knowledge/knowledge.jsonl` | L0-L3 分级路由检索源 |
| 固定输入 | `data/fixtures/voice.wav` | 16 kHz 单声道 16-bit 固定 WAV（32 KB） |
| 体检脚本 | `scripts/check_taishanpi3m.sh` | 板端只读体检（CPU/NPU/ALSA/负载），全程只读 |
| 无硬件依赖验收 | `scripts/check_no_hw_deps.sh` | ldd 逐二进制检查，默认构建不得链接 RKLLM/sherpa/onnx/asound |

## 明确排除

| 项 | 原因 |
|---|---|
| `models/`（模型与厂商 SDK） | 模型 1.3 GB 超仓库限制且再分发许可不明确；Rockchip SDK 许可禁止再分发 |
| `build-*` 构建目录 | 平台产物，按 `.gitignore` 不入库；板端用 `build-taishanpi3m` |
| `.git/` | 版本库不入部署包 |
| 原始运行日志 / 中间 WAV | 每板独立生成，不入包 |

## 复现步骤（板端）

```bash
# 1. 依赖：CMake ≥ 3.22、C++17 编译器、libzmq3-dev（4.3.x）；
#    nlohmann-json3-dev（3.10.x）缺失时构建自动回退仓库内单头文件
sudo apt install -y cmake g++ libzmq3-dev nlohmann-json3-dev

# 2. 构建（Release，目录 build-taishanpi3m）
bash deploy/taishanpi3m/build.sh

# 3. 全量测试（板端 CTest，与 WSL Mock 同一套用例）
ctest --test-dir build-taishanpi3m --output-on-failure

# 4. 无硬件依赖验收
bash scripts/check_no_hw_deps.sh build-taishanpi3m

# 5. 运行会话链（前台演示；日志与 WAV 输出见 /tmp/voxorchestra-session/）
bash deploy/taishanpi3m/run_mock_chain.sh
```

## 与 WSL Mock 的一致性

- 同一源码、同一 CMake 工程、同一套 CTest 用例（29 项）；板端构建即为
  aarch64 原生编译，无交叉编译差异。
- 默认构建不依赖 NPU SDK、厂商 Runtime 或声卡；Mock 阶段音频输入输出
  均为 WAV 文件，验证口径与 WSL 完全一致。
- 板端体检（`check_taishanpi3m.sh`）只采集事实（CPU/NPU/ALSA/负载），
  为板卡阶段接入真实后端建立基线，不判定硬件损坏。

## 板卡阶段入口（不在本包范围）

真实硬件后端按官方资料逐级验证：官方 Ubuntu 24.04 镜像（板卡已烧录）→ RKLLM
Demo/Runtime/预转模型（Model Zoo 指定型号）→ sherpa-onnx / SummerTTS
音频资源 → 项目 Backend 适配 → 真机全链路。模型、运行库与板端驱动由
官方渠道获取，不随仓库分发；基线先保 WAV 输入输出，再验证板载麦克风
与 3.5 mm 音频输出。
