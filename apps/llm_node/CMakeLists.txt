# 大模型生成节点：RuntimeNode 外壳 + ILlmBackend 工厂注入。
# 默认 FakeLlmBackend（Mock 文本负载）；硬件后端开启时接入 RKLLM。
add_executable(llm_node main.cpp)
target_link_libraries(llm_node PRIVATE voxorchestra::app_common voxorchestra::fake_backends)
# RKLLM 的选项/校验头（rkllm_options.hpp）不含厂商类型，两种构建都要能
# include：默认构建下节点仍解析并校验 session.json::llm.*，非法采样参数在
# 启动阶段报错；只有后端实现（rkllm_llm_backend.hpp）受 HW 选项保护。
target_include_directories(llm_node PRIVATE
  ${CMAKE_SOURCE_DIR}/backends/rkllm/include
)
if(VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS)
  target_compile_definitions(llm_node PRIVATE VOXORCHESTRA_HAS_RKLLM=1)
  target_link_libraries(llm_node PRIVATE voxorchestra::rkllm)
endif()
