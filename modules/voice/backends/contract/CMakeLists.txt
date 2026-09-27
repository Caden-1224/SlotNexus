# 语音后端契约（ASR/LLM/TTS/RAG/AudioSink/BackendEvent）。
#
# Author: Caden
add_library(slotnexus_voice_backend INTERFACE)
add_library(slotnexus::voice_backend ALIAS slotnexus_voice_backend)
# 兼容旧 target 名；新代码优先使用 slotnexus::voice_backend。
add_library(slotnexus::backend ALIAS slotnexus_voice_backend)
set_target_properties(slotnexus_voice_backend PROPERTIES EXPORT_NAME voice_backend)
target_include_directories(slotnexus_voice_backend INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(slotnexus_voice_backend INTERFACE cxx_std_17)
