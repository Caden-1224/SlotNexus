# 确定性 Fake 语音后端（header-only）。
#
# Author: Caden
add_library(slotnexus_voice_fake_backends INTERFACE)
add_library(slotnexus::voice_fake_backends ALIAS slotnexus_voice_fake_backends)
add_library(slotnexus::fake_backends ALIAS slotnexus_voice_fake_backends)
target_include_directories(slotnexus_voice_fake_backends INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(slotnexus_voice_fake_backends INTERFACE
  slotnexus::voice_backend)
target_compile_features(slotnexus_voice_fake_backends INTERFACE cxx_std_17)
