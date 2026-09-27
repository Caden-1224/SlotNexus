# 语音识别节点：通用 RuntimeNode 外壳 + 语音 IAsrBackend 适配器。
#
# Author: Caden
add_executable(asr_node main.cpp)
target_link_libraries(asr_node PRIVATE
  slotnexus::node_host
  slotnexus::voice_fake_backends
  slotnexus::voice_common)
if(SLOTNEXUS_ENABLE_HARDWARE_BACKENDS)
  target_compile_definitions(asr_node PRIVATE SLOTNEXUS_HAS_SHERTA_ONNX=1)
  target_link_libraries(asr_node PRIVATE slotnexus::sherpa_asr)
endif()
set_target_properties(asr_node PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/asr_node)
