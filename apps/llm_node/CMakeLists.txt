# Fake 大模型生成节点：RuntimeNode 外壳 + FakeLlmBackend 适配器。
add_executable(llm_node main.cpp)
target_link_libraries(llm_node PRIVATE voxorchestra::app_common voxorchestra::fake_backends)
