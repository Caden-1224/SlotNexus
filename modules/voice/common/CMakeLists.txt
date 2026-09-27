# 语音通用库：WAV 读取、分句与事件适配。
#
# Author: Caden
add_library(slotnexus_voice_common STATIC
  src/wav_reader.cpp
  src/sentence_chunker.cpp
)
add_library(slotnexus::voice_common ALIAS slotnexus_voice_common)
set_target_properties(slotnexus_voice_common PROPERTIES EXPORT_NAME voice_common)
target_include_directories(slotnexus_voice_common PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(slotnexus_voice_common PUBLIC
  slotnexus::common
  slotnexus::voice_backend
  slotnexus::runtime
  slotnexus::dataplane)
target_compile_features(slotnexus_voice_common PUBLIC cxx_std_17)
