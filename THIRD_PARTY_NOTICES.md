# 第三方组件与许可记录

本项目目前不打包任何第三方代码或二进制。

引入任何第三方组件（源码、库、模型、SDK、镜像脚本）前，必须在此记录：

| 组件 | 来源 | 版本 | 许可证 | 用途 | 允许复制到仓库 |
|---|---|---|---|---|---|
| nlohmann-json | apt 包 nlohmann-json3-dev | 3.10.5 | MIT | MessageEnvelope JSON 编解码 | 否（系统包，不打包） |
| ZeroMQ（libzmq） | apt 包 libzmq3-dev / libzmq5 | 4.3.4 | MPL-2.0 | 控制面 RPC 与数据面流 | 否（系统包，动态链接） |

规则：

- 只有许可证允许再分发的内容才能进入公开仓库；模型、厂商 SDK 与私有镜像一律只放 `models/README.md` / `third_party/README.md` 中的获取说明。
- 引入任何第三方代码前必须先核验许可证；核验记录见 `artifacts/environment-preflight/source-reuse-ledger.md`。
