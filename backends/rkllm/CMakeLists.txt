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
