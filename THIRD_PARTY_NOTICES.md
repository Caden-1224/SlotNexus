# 第三方组件与许可记录

已打包的第三方组件：nlohmann-json 单头文件（MIT，`third_party/nlohmann/json.hpp`，许可头保留在文件内）；其余以系统包方式引入，不打包进仓库。

引入任何第三方组件（源码、库、模型、SDK、镜像脚本）前，必须在此记录：

| 组件 | 来源 | 版本 | 许可证 | 用途 | 允许复制到仓库 |
|---|---|---|---|---|---|
| nlohmann-json | 仓库 `third_party/nlohmann/json.hpp`（源自 nlohmann-json3-dev） | 3.10.5 | MIT | MessageEnvelope JSON 编解码 | 是（单头文件，许可头已保留） |
| ZeroMQ（libzmq） | apt 包 libzmq3-dev / libzmq5 | 4.3.4 | MPL-2.0 | 控制面 RPC 与数据面流 | 否（系统包，动态链接） |
| SummerTTS（vits） | 作者仓库 `tts/`（板端源码编译，不入库） | vits-based（2024-12-14 声明） | MIT | TTS 后端推理（`backends/summer_tts`） | 否（板端源码编译，仅 Eigen 依赖） |

规则：

- 只有许可证允许再分发的内容才能进入公开仓库；模型、厂商 SDK 与私有镜像一律只放 `models/README.md` / `third_party/README.md` 中的获取说明。
- 引入任何第三方代码前必须先核验许可证；核验记录见 `artifacts/environment-preflight/source-reuse-ledger.md`。
