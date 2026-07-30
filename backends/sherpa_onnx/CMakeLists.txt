# sherpa-onnx 语音识别后端（板端 aarch64；仅 VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=ON 时构建）。
#
# 上游源码不随仓库分发（third_party/README.md 约定），构建时由
# VOXORCHESTRA_SHERTA_ROOT 指向板端源码目录（如
# ~/workspace/upstream_rkllm/sherpa-onnx，含 sherpa-onnx/c-api/c-api.h 与
# build/lib/libsherpa-onnx-c-api.so）。onnxruntime 由 sherpa-onnx 构建时拉取
# 到 build/_deps/onnxruntime-src/lib（共享库的 DT_NEEDED，运行时经
# LD_LIBRARY_PATH 提供，与本库链接无关）。模型文件不入库，
# 路径经 config/taishanpi3m/session.json::asr.model 或 asr_node --model 传入。
set(VOXORCHESTRA_SHERTA_ROOT ""
    CACHE PATH "sherpa-onnx 上游源码根目录（含 sherpa-onnx/ 与 build/）")
if(NOT VOXORCHESTRA_SHERTA_ROOT)
  message(FATAL_ERROR
    "VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=ON 需要 "
    "-DVOXORCHESTRA_SHERTA_ROOT=<sherpa-onnx 源码根目录>")
endif()

add_library(voxorchestra_sherpa_asr STATIC src/sherpa_asr_backend.cpp)
add_library(voxorchestra::sherpa_asr ALIAS voxorchestra_sherpa_asr)

# 契约头（backend_event.hpp / i_asr_backend.hpp）是本库的公共 API，必须
# 向消费者传播（sherpa_asr_test / asr_node 都要 include）；sherpa-onnx 的
# C API 仅内部使用，保持 PRIVATE（静态库的 PRIVATE 链接依赖由 CMake 以
# $<LINK_ONLY> 传播给消费者，asr_node 链接时仍会带上 -lsherpa-onnx-c-api）。
target_link_libraries(voxorchestra_sherpa_asr PUBLIC voxorchestra::backend)
target_link_libraries(voxorchestra_sherpa_asr PRIVATE
  ${VOXORCHESTRA_SHERTA_ROOT}/build/lib/libsherpa-onnx-c-api.so)
target_include_directories(voxorchestra_sherpa_asr PRIVATE
  ${VOXORCHESTRA_SHERTA_ROOT}
)
target_include_directories(voxorchestra_sherpa_asr PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(voxorchestra_sherpa_asr PUBLIC cxx_std_17)
