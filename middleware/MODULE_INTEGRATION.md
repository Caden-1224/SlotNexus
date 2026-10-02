# 新模块接入指南

本文说明如何在 `modules/<name>/` 中新增一个业务模块。中间件只提供通用协议、通信、任务运行时和服务外壳；业务类型、模型 SDK、音频设备和模块私有配置都属于模块自身。

## 目录约定

```text
modules/<name>/
  CMakeLists.txt
  common/                  模块内共享类型和纯业务工具
  backends/                Backend 适配和厂商 SDK 隔离
  nodes/
    <name>_node/           一个可部署的任务节点
      CMakeLists.txt
      main.cpp
  config/                  Manager 配置和运行参数
  tests/                   模块单元、集成和端到端测试
```

规模较小的模块可以省略不需要的目录，但仍放在 `modules/<name>/` 下。不要把模块目录加入 `middleware/CMakeLists.txt` 的库实现，也不要让 middleware 头文件包含模块头文件。

## 可用的 Core target

| 能力 | CMake target | 主要入口 | 使用场景 |
| --- | --- | --- | --- |
| 基础工具 | `slotnexus::common` | `slotnexus/common/*` | 队列、日志、Base64、版本 |
| 消息协议 | `slotnexus::protocol` | `MessageEnvelope` | 所有跨进程请求和响应 |
| 通信原语 | `slotnexus::transport` | `RpcClient`、`PubSocket` / `SubSocket`、Push/Pull | RPC、事件和流式数据传输 |
| 任务执行 | `slotnexus::runtime` | `IBackend`、`TaskRuntime` | 实现一个计算节点 |
| 节点外壳 | `slotnexus::node_host` | `RuntimeNode` | 把 Backend 暴露为标准节点进程 |
| 中间事件 | `slotnexus::dataplane` | `DataplaneEvent`、事件通道 | token、音频帧等非最终结果 |
| TCP 网络 | `slotnexus::network` | `TcpServer`、`EventLoop` | 只有新增 TCP 服务时使用 |
| 任务注册 | `slotnexus::task_registry` | `TaskRegistry` | 通常由 Unit Manager 使用 |

常规模块节点通常只需要链接 `slotnexus::node_host`。它会传递所需的 runtime、protocol、transport 和 dataplane 依赖。需要流式事件时再直接使用 `slotnexus::dataplane`；不要为了调用 Backend 而直接依赖 `network`。

## Backend 接入

每个节点实现 `slotnexus::runtime::IBackend`。Backend 负责业务输入、取消和超时检查、最终结果，以及可选的中间事件；它不负责 work_id 路由和进程级 RPC。

```cpp
class MyBackend final : public slotnexus::runtime::IBackend {
 public:
  slotnexus::runtime::BackendResult infer(
      const nlohmann::json& input,
      std::chrono::steady_clock::time_point deadline,
      const std::atomic<bool>& cancelled,
      const slotnexus::runtime::EventSink& events) override {
    // 校验模块自己的 input payload，执行模型或业务逻辑。
    if (cancelled.load()) {
      return {slotnexus::runtime::BackendResult::Code::kCancelled, {}};
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return {slotnexus::runtime::BackendResult::Code::kTimeout, {}};
    }
    // 长任务在执行过程中继续检查 deadline / cancelled；中间事件通过 events 发布。
    return {slotnexus::runtime::BackendResult::Code::kOk,
            {{"result", "..."}}};
  }
};
```

节点入口使用 `RuntimeNode` 连接标准控制协议和 Backend 工厂。下例假设 `stopping` 是由应用退出逻辑设置的 `std::atomic<bool>`，并已定义上面的 `MyBackend`：

```cpp
zmq::context_t context(1);
auto runtime = std::make_unique<slotnexus::runtime::TaskRuntime>(
    [] { return std::make_shared<MyBackend>(); });
slotnexus::node::RuntimeNode node(context, std::move(runtime));
node.bind("tcp://127.0.0.1:19220");
while (!stopping.load()) {
  node.serve_once(std::chrono::milliseconds(100));
}
node.close();
```

真实模型或厂商 SDK 放在 `backends/<provider>/`，通过模块自己的 target 隔离。默认构建应提供 Fake Backend，使协议、任务和节点生命周期可以在没有硬件的环境中验证。

## 请求和事件约定

- `MessageEnvelope` 的外层字段由 Core 处理；模块只定义自己的 `payload` 内容。
- 每个模块使用稳定的 `module_id`，并在 Manager 配置中登记节点端点和协议版本。
- `BackendEvent::type` 会映射为数据面的事件 `kind`，由模块定义并由消费者一致解释。当前语音使用 `partial`、`final`、`token`、`pcm`、`done`；新增模块可约定命名空间前缀，但 Core 不强制添加前缀。
- 最终结果通过 BackendResult 返回；需要低延迟或连续输出的内容才发布 dataplane 事件。
- 不要把模型 SDK 类型、音频设备句柄或模块业务结构体加入 middleware 公共头。

## CMake 模板

模块独立构建时先安装 Core，再通过 `CMAKE_PREFIX_PATH` 查找包：

```cmake
cmake_minimum_required(VERSION 3.22)
project(slotnexus_<name> VERSION 0.1.0 LANGUAGES CXX)

find_package(slotnexus-core CONFIG REQUIRED)

add_executable(<name>_node nodes/<name>_node/main.cpp)
target_link_libraries(<name>_node PRIVATE slotnexus::node_host)
target_compile_features(<name>_node PRIVATE cxx_std_17)
```

仓库顶层构建时，模块由自己的 `CMakeLists.txt` 管理，直接使用已经存在的 `slotnexus::...` targets。模块 target 命名建议使用 `slotnexus_<name>_*`，避免和 Core target 冲突。

`middleware/services/include/slotnexus/services/` 中只放多个通用服务共享的控制面辅助代码；模块不应依赖这些服务内部助手来定义自己的业务协议。

## 接入步骤

1. 创建 `modules/<name>/` 和模块自己的 CMake 入口。
2. 定义请求 `payload`、最终结果和事件 `kind`。
3. 实现 `IBackend`，先完成 Fake Backend 和取消路径。
4. 用 `RuntimeNode` 创建节点进程，并只链接必要的 Core target。
5. 在 `config/manager-modules.json` 中登记 `module_id`、协议版本和节点端点。
6. 添加模块单元测试和至少一个跨进程集成测试。
7. 先构建 Core 和模块的默认配置，再按部署平台启用真实 SDK。

## 验证命令

核心独立安装后，模块可以作为消费者构建：

```bash
cmake -S . -B build-core -DVOX_BUILD_VOICE=OFF -DVOX_BUILD_TESTS=ON
cmake --build build-core
cmake --install build-core --prefix /tmp/slotnexus-core
cmake -S modules/<name> -B build-<name> \
  -DCMAKE_PREFIX_PATH=/tmp/slotnexus-core
cmake --build build-<name>
```

跨层依赖检查应保持通过：Core 不得出现 `modules/`、模型 SDK 或音频设备专有类型的反向依赖。
