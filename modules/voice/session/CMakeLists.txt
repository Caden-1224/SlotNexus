# 语音 Session 编排库：状态机与 ASR→RAG→LLM→TTS→音频输出管线。
#
# Author: Caden
add_library(slotnexus_voice_session STATIC
  src/session_pipeline.cpp
  src/streaming_input.cpp
)
add_library(slotnexus::voice_session ALIAS slotnexus_voice_session)
add_library(slotnexus::session ALIAS slotnexus_voice_session)
set_target_properties(slotnexus_voice_session PROPERTIES EXPORT_NAME voice_session)
target_include_directories(slotnexus_voice_session PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(slotnexus_voice_session PUBLIC
  slotnexus::common
  slotnexus::voice_common
  slotnexus::voice_rag
  slotnexus::voice_backend)
target_compile_features(slotnexus_voice_session PUBLIC cxx_std_17)
