# 远端语音节点代理后端。
#
# Author: Caden
add_library(slotnexus_voice_net_backends STATIC
  src/net_backend_session.cpp
)
add_library(slotnexus::voice_net_backends ALIAS slotnexus_voice_net_backends)
add_library(slotnexus::net_backends ALIAS slotnexus_voice_net_backends)
target_include_directories(slotnexus_voice_net_backends PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(slotnexus_voice_net_backends PUBLIC
  slotnexus::voice_backend
  slotnexus::voice_common
  slotnexus::dataplane
  slotnexus::transport
  slotnexus::protocol)
target_compile_features(slotnexus_voice_net_backends PUBLIC cxx_std_17)
