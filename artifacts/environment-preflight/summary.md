# environment-preflight 摘要

> 日期：2026-07-25

## 最终运行了什么

- WSL（Ubuntu 22.04.5 LTS）环境探测，结果见 `versions.txt`；探测命令见 `commands.md`。
- 仓库骨架的构建冒烟测试（`ctest --preset wsl-debug`，1/1 通过，见构建记录）。
- 未运行任何模型或板卡测试（板卡尚未接入）。

## 哪些是 Fake / 官方数据 / 实测

- 编译器、CMake、ZeroMQ 版本：**实测**（versions.txt 原始输出）。
- 板卡信息：**未采集**（板卡未接入，本次只准备体检脚本与连接步骤）。

## 关键事实（版本链起点）

| 项 | 值 |
|---|---|
| 开发机 OS | Ubuntu 22.04.5 LTS（WSL2，内核 6.6.36.6-microsoft-standard-WSL2+） |
| 架构 | x86_64（i7-14650HX，24 逻辑核，15 GiB 内存） |
| 编译器 | gcc/g++ 11.4.0 |
| CMake | 3.22.1（Unix Makefiles，preset schema v3） |
| ZeroMQ | libzmq3-dev/libzmq5 4.3.4-2（zmq.h 就绪） |
| 磁盘 | 952 GiB 可用（/） |

> 版本链（板卡 -> 镜像 -> 内核 -> NPU 驱动 -> Runtime -> 模型）在板卡接入后补全，见 `scripts/check_taishanpi3m.sh` 输出。

## 失败在哪里

- 无失败项。探测脚本初版使用 pkg-config 探测 ZeroMQ 失败（pkg-config 未安装），已改为 dpkg 查询，重跑通过。

## 后续任务

- 三种 ZMQ 模式（REQ/REP、PUB/SUB、PUSH/PULL）测试；
- MessageEnvelope 消息基线；
- 官方 Ubuntu 24.04 镜像（板卡已烧录）/ 烧录工具 / RKLLM Demo/Runtime/模型的下载 manifest；
- 板卡体检脚本待用户提供 SSH/ADB 后执行。
