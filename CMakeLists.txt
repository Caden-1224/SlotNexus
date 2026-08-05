cmake_minimum_required(VERSION 3.22)

project(voxorchestra
  VERSION 0.1.0
  DESCRIPTION "VoxOrchestra: 基于 C++ 多进程推理框架的端侧全离线语音交互系统"
  LANGUAGES CXX)

# 统一使用 C++17：本项目所有库与可执行文件共用的语言标准。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "构建类型" FORCE)
endif()

# 硬件 Backend（RKLLM/sherpa-onnx/SummerTTS/ALSA）默认关闭，
# 保证 x86 开发环境不需要任何厂商 SDK 或声卡即可构建和测试。
option(VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS
  "构建硬件 Backend（需要厂商 SDK 与目标硬件）" OFF)

enable_testing()

add_subdirectory(libs/common)
add_subdirectory(libs/protocol)
add_subdirectory(libs/transport)
add_subdirectory(libs/dataplane)
add_subdirectory(libs/network)
add_subdirectory(libs/task_registry)
add_subdirectory(libs/runtime)
add_subdirectory(libs/rag)
add_subdirectory(libs/session)
add_subdirectory(backends)
add_subdirectory(apps)
add_subdirectory(tests/unit)
add_subdirectory(tests/integration)
add_subdirectory(tests/e2e)
add_subdirectory(tests/fault)
