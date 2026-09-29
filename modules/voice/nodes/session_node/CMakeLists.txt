# 会话编排节点：语音 Session 管线 + RAG + 可选 ALSA。
#
# Author: Caden
add_library(voice_session_node_lib STATIC session_node.cpp)
add_library(slotnexus::voice_session_node_lib ALIAS voice_session_node_lib)
target_include_directories(voice_session_node_lib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(voice_session_node_lib PRIVATE
  slotnexus::transport
  slotnexus::voice_session
  slotnexus::voice_rag
  slotnexus::voice_fake_backends
  slotnexus::voice_net_backends
  slotnexus::protocol)
if(SLOTNEXUS_ENABLE_HARDWARE_BACKENDS)
  target_compile_definitions(voice_session_node_lib PRIVATE SLOTNEXUS_HAS_ALSA=1)
  target_compile_definitions(voice_session_node_lib PRIVATE SLOTNEXUS_HAS_SHERTA_ONNX=1)
  target_link_libraries(voice_session_node_lib PRIVATE slotnexus::alsa)
  target_link_libraries(voice_session_node_lib PRIVATE slotnexus::sherpa_asr)
endif()
add_executable(session_node main.cpp)
target_link_libraries(session_node PRIVATE
  voice_session_node_lib
  slotnexus::transport
  slotnexus::voice_session
  slotnexus::voice_rag
  slotnexus::voice_fake_backends
  slotnexus::voice_net_backends
  slotnexus::protocol)
if(SLOTNEXUS_ENABLE_HARDWARE_BACKENDS)
  target_compile_definitions(session_node PRIVATE SLOTNEXUS_HAS_ALSA=1)
endif()
set_target_properties(session_node PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/session_node)
