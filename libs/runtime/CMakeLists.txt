# 节点运行时：任务状态机（setup/inference/cancel/taskinfo/exit）与模拟 Backend。
# 未来 RKLLM / sherpa-onnx / SummerTTS 等硬件后端实现同一 IBackend 接口即可接入。
add_library(slotnexus_runtime STATIC
  src/task_channel.cpp
  src/task_runtime.cpp
)
add_library(slotnexus::runtime ALIAS slotnexus_runtime)

target_include_directories(slotnexus_runtime PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# slotnexus::backend 为纯头契约库（BackendEvent），无条件构建。
target_link_libraries(slotnexus_runtime PUBLIC
  slotnexus::backend slotnexus::task_registry
)
target_compile_features(slotnexus_runtime PUBLIC cxx_std_17)
