# 语音合成节点：通用 RuntimeNode 外壳 + 语音 ITtsBackend/AudioSink 适配器。
#
# Author: Caden
add_executable(tts_node main.cpp)
target_link_libraries(tts_node PRIVATE
  slotnexus::node_host
  slotnexus::voice_fake_backends
  slotnexus::voice_common)
if(SLOTNEXUS_ENABLE_HARDWARE_BACKENDS)
  target_compile_definitions(tts_node PRIVATE SLOTNEXUS_HAS_MELOTTS=1)
  target_compile_definitions(tts_node PRIVATE SLOTNEXUS_HAS_ALSA=1)
  target_link_libraries(tts_node PRIVATE slotnexus::melotts slotnexus::alsa)
endif()
set_target_properties(tts_node PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/tts_node)
