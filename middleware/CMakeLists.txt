# 通用中间件核心构建入口。
#
# Author: Caden
# 组织方式：common/json/protocol/transport/network/task_registry/runtime/
# dataplane 为可安装库；services 为通用 node host、unit manager、gateway、
# 设计要求：所有公开头不得依赖语音模块目录或音频专有类型。
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# 内置 nlohmann/json（MIT，third_party/nlohmann/json.hpp）。核心公开头使用
# <nlohmann/json.hpp>，通过该 interface target 同时支持构建树与安装树。
add_library(slotnexus_json INTERFACE)
add_library(slotnexus::json ALIAS slotnexus_json)
set_target_properties(slotnexus_json PROPERTIES EXPORT_NAME json)
target_compile_features(slotnexus_json INTERFACE cxx_std_17)
target_include_directories(slotnexus_json INTERFACE
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/third_party>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
install(TARGETS slotnexus_json EXPORT slotnexus-core-targets)
install(FILES ${CMAKE_SOURCE_DIR}/third_party/nlohmann/json.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nlohmann)

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
