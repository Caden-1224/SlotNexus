# VoxOrchestra/SlotNexus 顶层构建入口。
#
# Author: Caden
# 设计意图：同一仓库内分离“通用中间件核心”和“语音应用模块”。核心可在
# VOX_BUILD_VOICE=OFF 下独立配置/构建/测试/安装；语音只通过公开 target 或
# find_package(slotnexus-core) 消费核心，不反向被核心依赖。
#
# 主要开关：
#   VOX_BUILD_VOICE          构建 modules/voice（默认 ON）
#   VOX_BUILD_TESTS          构建测试（默认 ON）
#   SLOTNEXUS_ENABLE_HARDWARE_BACKENDS 构建板端真实语音后端（默认 OFF）
cmake_minimum_required(VERSION 3.22)

project(slotnexus
  VERSION 0.2.0
  DESCRIPTION "SlotNexus: 通用多进程通信与推理中间件；voice 为首个可选应用模块"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "构建类型" FORCE)
endif()

option(VOX_BUILD_VOICE "构建语音应用模块 modules/voice" ON)
option(VOX_BUILD_TESTS "构建测试" ON)
option(SLOTNEXUS_ENABLE_HARDWARE_BACKENDS
  "构建硬件语音后端（需要厂商 SDK 与目标硬件）" OFF)

include(GNUInstallDirs)
enable_testing()

add_subdirectory(middleware)

if(VOX_BUILD_VOICE)
  add_subdirectory(modules/voice)
endif()

if(VOX_BUILD_TESTS)
  add_subdirectory(tests/core)
  if(VOX_BUILD_VOICE)
    add_subdirectory(modules/voice/tests)
  endif()
endif()
