# 大模型生成节点：RuntimeNode 外壳 + ILlmBackend 工厂注入。
# 默认 FakeLlmBackend（Mock 文本负载）；硬件后端开启时接入 RKLLM。
add_executable(llm_node main.cpp)
target_link_libraries(llm_node PRIVATE voxorchestra::app_common voxorchestra::fake_backends)
if(VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS)
  target_compile_definitions(llm_node PRIVATE VOXORCHESTRA_HAS_RKLLM=1)
  target_link_libraries(llm_node PRIVATE voxorchestra::rkllm)
endif()