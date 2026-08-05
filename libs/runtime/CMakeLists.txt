# 节点运行时：任务状态机（setup/inference/cancel/taskinfo/exit）与模拟 Backend。
# 未来 RKLLM / sherpa-onnx / SummerTTS 等硬件后端实现同一 IBackend 接口即可接入。
add_library(voxorchestra_runtime STATIC
  src/task_channel.cpp
  src/task_runtime.cpp
)
add_library(voxorchestra::runtime ALIAS voxorchestra_runtime)

target_include_directories(voxorchestra_runtime PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# voxorchestra::backend 为纯头契约库（BackendEvent），无条件构建。
target_link_libraries(voxorchestra_runtime PUBLIC
  voxorchestra::backend voxorchestra::task_registry
)
target_compile_features(voxorchestra_runtime PUBLIC cxx_std_17)
