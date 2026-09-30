# 通用中间件核心构建入口。
#
# Author: Caden
# 组织方式：common/json/protocol/transport/network/task_registry/runtime/
# dataplane 为可安装库；services 同时包含通用服务库和对应的进程入口。
# 新业务模块放在 modules/<name>/，接入步骤见 middleware/MODULE_INTEGRATION.md。
# 设计要求：所有公开头不得依赖语音模块目录或音频专有类型。
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# 内置 nlohmann/json 3.10.5 完整头文件集合（MIT，third_party/nlohmann/）。
# 不能只内嵌顶层 json.hpp：multi-header 版会继续 include nlohmann/detail/...，
# 缺少子头时会被系统其它版本的 nlohmann 头污染，造成版本混用编译错误。
# 核心公开头使用 <nlohmann/json.hpp>，通过该 interface target 同时支持构建树与安装树。
add_library(slotnexus_json INTERFACE)
add_library(slotnexus::json ALIAS slotnexus_json)
set_target_properties(slotnexus_json PROPERTIES EXPORT_NAME json)
target_compile_features(slotnexus_json INTERFACE cxx_std_17)
target_include_directories(slotnexus_json INTERFACE
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
install(TARGETS slotnexus_json EXPORT slotnexus-core-targets)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/nlohmann
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp")

add_subdirectory(common)
add_subdirectory(protocol)
add_subdirectory(transport)
add_subdirectory(network)
add_subdirectory(task_registry)
add_subdirectory(runtime)
add_subdirectory(dataplane)
add_subdirectory(services)

# Core package: find_package(slotnexus-core CONFIG REQUIRED)
set(SLOTNEXUS_CORE_CMAKE_DIR
    "${CMAKE_INSTALL_LIBDIR}/cmake/slotnexus-core")
install(EXPORT slotnexus-core-targets
        FILE slotnexus-core-targets.cmake
        NAMESPACE slotnexus::
        DESTINATION ${SLOTNEXUS_CORE_CMAKE_DIR})
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/slotnexus-core-config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/slotnexus-core-config.cmake
  INSTALL_DESTINATION ${SLOTNEXUS_CORE_CMAKE_DIR})
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/slotnexus-core-config-version.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/slotnexus-core-config.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/slotnexus-core-config-version.cmake
  DESTINATION ${SLOTNEXUS_CORE_CMAKE_DIR})
