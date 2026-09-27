# 大模型生成节点：通用 RuntimeNode 外壳 + 语音 ILlmBackend 适配器。
#
# Author: Caden
add_executable(llm_node main.cpp)
target_link_libraries(llm_node PRIVATE
  slotnexus::node_host
  slotnexus::voice_fake_backends
  slotnexus::voice_common)
target_include_directories(llm_node PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../backends/rkllm/include)
if(SLOTNEXUS_ENABLE_HARDWARE_BACKENDS)
  target_compile_definitions(llm_node PRIVATE SLOTNEXUS_HAS_RKLLM=1)
  target_link_libraries(llm_node PRIVATE slotnexus::rkllm)
endif()
set_target_properties(llm_node PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/llm_node)
