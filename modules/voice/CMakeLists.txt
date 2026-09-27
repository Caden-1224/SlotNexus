# 语音应用模块构建入口。
#
# Author: Caden
# 设计意图：modules/voice 只通过中间件公开 target 或已安装的
# find_package(slotnexus-core CONFIG) 消费核心；核心不依赖本目录。
# 支持两种用法：
#   1. 顶层统一构建：cmake -S . -DVOX_BUILD_VOICE=ON ...
#   2. 独立消费者构建：
#      cmake --install <core-build> --prefix /opt/vox
#      cmake -S modules/voice -B build-voice -DCMAKE_PREFIX_PATH=/opt/vox
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.22)
  project(slotnexus_voice VERSION 0.1.0 LANGUAGES CXX)
  include(GNUInstallDirs)
  find_package(slotnexus-core CONFIG REQUIRED)
  if(NOT DEFINED VOX_BUILD_TESTS)
    set(VOX_BUILD_TESTS OFF CACHE BOOL "构建语音模块测试（需要核心服务可执行文件路径）")
  endif()
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  enable_testing()
else()
  # 作为顶层 add_subdirectory 参与统一构建；核心 target 已在作用域中。
endif()

add_subdirectory(backends)
add_subdirectory(common)
add_subdirectory(rag)
add_subdirectory(session)
add_subdirectory(nodes)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND VOX_BUILD_TESTS)
  add_subdirectory(tests)
endif()
