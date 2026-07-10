# 源码复用台账（source-reuse-ledger）

> 核验日期：2026-07-10
> 对象：`开源参考/Edge-LLM-Infra` 与 `开源参考/LLM_Voice_Flow`
> 结论先行：**两个参考工程的“自有代码”都没有明确的许可证文件或许可证头，一律只研究接口与实现思路，不得复制源码到 `voxorchestra-runtime/`。** 各自内嵌的第三方组件许可证清晰，可作为外部依赖引入并在 `THIRD_PARTY_NOTICES.md` 登记。

## 1. Edge-LLM-Infra

| 项 | 结果 |
|---|---|
| 仓库来源 | 本地拷贝，无 `.git` 元数据，无远端地址 |
| 顶层 LICENSE | **无** |
| 自有代码（hybrid-comm/network/node/infra-controller/unit-manager/utils/sample） | 源码头部无 SPDX 或版权声明，许可证不明确 |
| 内嵌第三方 | `hybrid-comm/include/libzmq/zmq.h`（MPL-2.0，ZeroMQ 官方头）；`pzmq_data.h/cpp`（MIT，M5Stack Technology，SPDX-FileCopyrightText: 2024） |

**可复用**：架构概念 —— 控制面 RPC + 数据面 Channel、Unit Manager 的 work_id 生命周期、epoll Reactor 思路。这些属于设计思想，本项目按自己需求独立实现。
**不可复制**：所有自有源码；也不要把 `libzmq/` 整目录拷进仓库（系统已提供 `libzmq5-dev` 的头与库，直接 `#include <zmq.h>`）。

## 2. LLM_Voice_Flow

| 项 | 结果 |
|---|---|
| 仓库来源 | `https://github.com/superxiaobai-1/LLM_Voice_Flow.git`（本地有 `.git`，远端 origin 确认） |
| 顶层 LICENSE | **无** |
| 自有代码（zmq-comm-kit/、tts/tts_server/、voice/ 集成层、llm/ 集成层） | 源码头部无许可证声明，许可证不明确 |
| 内嵌第三方 | sherpa-onnx（Apache-2.0，`voice/sherpa-onnx/LICENSE`）；rknn-llm（Rockchip 自定义 BSD 风格许可，含第 4 条“含开源组件时须遵守其许可证”，`llm/rknn-llm/LICENSE`）；SummerTTS（MIT，`tts/README.md` 自述）；Eigen 3.4.0（多许可：MPL2/BSD/Apache/GPL/LGPL，按组件适用）；gflags/glog（BSD，`tts/src/tn/`） |

**可复用**：
- SummerTTS（MIT）：可复制/修改，但本项目以“官方 Demo + Adapter”方式集成，代码独立实现，模型从作者网盘获取（不在仓库内）。
- sherpa-onnx（Apache-2.0）：作为外部依赖引入，遵守 NOTICE 要求。
- rknn-llm（Rockchip 许可）：只作为板上运行依赖（`librkllmrt.so` + 官方 Demo 编译运行），不把其源码或产物提交到公开仓库。
- Eigen/gflags/glog：第三方依赖，按各自许可使用。

**不可复制**：LLM_Voice_Flow 自有集成代码（zmq-comm-kit、tts_server 双缓冲队列、voice 服务层、llm 服务层）——无许可证。其中“双缓冲队列”“模块化通信”等设计思路本项目独立实现。

## 3. 决策记录（2026-07-10）

两个参考工程的自有代码均无明确许可证，本项目仅借鉴其架构与接口思路（控制面/数据面分离、work_id 生命周期、双缓冲队列、RPC+流式通道），代码全部独立编写；有明确许可证的第三方组件（SummerTTS MIT、sherpa-onnx Apache-2.0、ZeroMQ MPL-2.0）按许可证要求引入。

## 4. 对 voxorchestra-runtime 的落地规则

1. 不把两个参考工程的任何自有源码文件拷入仓库；需要时只参考接口形态与行为，然后独立编写。
2. ZeroMQ 使用系统包 `libzmq5-dev`（MPL-2.0 例外条款允许动态链接使用），不拷贝 zmq.h 进仓库。
3. 板上真实 Backend 只以“外部依赖 + 官方 Demo 证据”方式接入：sherpa-onnx（Apache-2.0）、rkllm Demo（Rockchip 许可）、SummerTTS（MIT），每个在 `THIRD_PARTY_NOTICES.md` 登记版本与用途。
4. 所有第三方组件清单随下载 manifest 同步更新。
