# RKLLM 大模型生成后端（板端 aarch64；仅 VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=ON 时构建）。
#
# 上游源码不随仓库分发（third_party/README.md 约定），构建时由
# VOXORCHESTRA_RKLLM_ROOT 指向板端 SDK 目录（如
# ~/workspace/upstream_rkllm/rknn-llm/rkllm-runtime/Linux/librkllm_api/，
# 含 include/rkllm.h 与 aarch64/librkllmrt.so）。librkllmrt.so 的
# DT_NEEDED 为 libgomp.so.1（板卡 gcc 自带）；运行时解析 librkllmrt.so
# 本身需 LD_LIBRARY_PATH 指向其所在目录（与板端部署约束一致）。模型文件
# 不入库，路径经 config/taishanpi3m/session.json::llm.model 或
# llm_node --model 传入。
set(VOXORCHESTRA_RKLLM_ROOT ""
    CACHE PATH "RKLLM SDK 根目录（含 include/rkllm.h 与 aarch64/librkllmrt.so）")
if(NOT VOXORCHESTRA_RKLLM_ROOT)
  message(FATAL_ERROR
    "VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=ON 需要 "
    "-DVOXORCHESTRA_RKLLM_ROOT=<RKLLM SDK 根目录>")
endif()

add_library(voxorchestra_rkllm STATIC src/rkllm_llm_backend.cpp)
add_library(voxorchestra::rkllm ALIAS voxorchestra_rkllm)

# 契约头（backend_event.hpp / i_llm_backend.hpp）是本库的公共 API，必须
# 向消费者传播（rkllm_llm_test / llm_node 都要 include）；rkllm.h 仅内部
# 使用，保持 PRIVATE（静态库的 PRIVATE 链接依赖由 CMake 以 $<LINK_ONLY>
# 传播给消费者，llm_node 链接时仍会带上 -lrkllmrt）。
target_link_libraries(voxorchestra_rkllm PUBLIC voxorchestra::backend)
target_link_libraries(voxorchestra_rkllm PRIVATE
  ${VOXORCHESTRA_RKLLM_ROOT}/aarch64/librkllmrt.so)
target_include_directories(voxorchestra_rkllm PRIVATE
  ${VOXORCHESTRA_RKLLM_ROOT}/include
)
target_include_directories(voxorchestra_rkllm PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(voxorchestra_rkllm PUBLIC cxx_std_17)

# rknn-llm 在 1.2.0（build 2025-04-08）之后调整过 rkllm_init 的第三个参数：
#   - 旧形态：裸函数指针 void (*)(RKLLMResult*, void*, LLMCallState)
#   - 新形态：RKLLMCallback*（result_callback + result_userdata，回调返回 int）
# 头文件里没有版本宏，板端两种 SDK 都可能存在，因此这里用试编译探测接口
# 形态，而不是要求部署者记住自己手里是哪一版；两种都不匹配时直接失败，
# 避免把"头文件不配套"误报成语法错误。
include(CheckCXXSourceCompiles)
set(_rkllm_saved_try_compile_target_type ${CMAKE_TRY_COMPILE_TARGET_TYPE})
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_REQUIRED_INCLUDES ${VOXORCHESTRA_RKLLM_ROOT}/include)

check_cxx_source_compiles("
#include <cstddef>
#include <rkllm.h>
int cb(RKLLMResult*, void*, LLMCallState) { return 0; }
int main() {
  LLMHandle handle = nullptr;
  RKLLMParam param = rkllm_createDefaultParam();
  RKLLMCallback callback = {};
  callback.result_callback = &cb;
  callback.result_userdata = nullptr;
  return rkllm_init(&handle, &param, &callback);
}" VOXORCHESTRA_RKLLM_INIT_CALLBACK_STRUCT)

check_cxx_source_compiles("
#include <cstddef>
#include <rkllm.h>
void cb(RKLLMResult*, void*, LLMCallState) {}
int main() {
  LLMHandle handle = nullptr;
  RKLLMParam param = rkllm_createDefaultParam();
  return rkllm_init(&handle, &param, &cb);
}" VOXORCHESTRA_RKLLM_INIT_FUNCTION_POINTER)

set(CMAKE_TRY_COMPILE_TARGET_TYPE ${_rkllm_saved_try_compile_target_type})
unset(CMAKE_REQUIRED_INCLUDES)

if(VOXORCHESTRA_RKLLM_INIT_CALLBACK_STRUCT)
  message(STATUS "RKLLM 接口形态: rkllm_init(RKLLMCallback*)（1.2.0 之后的 rkllm.h）")
  target_compile_definitions(voxorchestra_rkllm
    PRIVATE VOXORCHESTRA_RKLLM_INIT_CALLBACK_STRUCT=1)
elseif(VOXORCHESTRA_RKLLM_INIT_FUNCTION_POINTER)
  message(STATUS "RKLLM 接口形态: rkllm_init(裸函数指针)（1.2.0 build 2025-04-08 的 rkllm.h）")
else()
  message(FATAL_ERROR
    "无法识别 rkllm_init 的接口形态，请检查 "
    "${VOXORCHESTRA_RKLLM_ROOT}/include/rkllm.h 是否为 RKLLM Runtime 头文件")
endif()
