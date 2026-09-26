# 会话编排节点：Fake 后端 + 真实 BM25 路由 + 有界队列 + generation 取消。
# 依赖 slotnexus::transport 引入 libzmq（ROUTER 服务端）。
add_library(session_node_lib STATIC
  session_node.cpp
)
target_include_directories(session_node_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(session_node_lib PRIVATE
  slotnexus::transport
  slotnexus::session
  slotnexus::rag
  slotnexus::fake_backends
  slotnexus::net_backends
  slotnexus::protocol
)
# 现场麦克风输入（mode=alsa）：硬件后端开启时接入 AlsaAudioSource。
if(SLOTNEXUS_ENABLE_HARDWARE_BACKENDS)
  target_compile_definitions(session_node_lib PRIVATE SLOTNEXUS_HAS_ALSA=1)
  target_link_libraries(session_node_lib PRIVATE slotnexus::alsa)
endif()

add_executable(session_node main.cpp)
target_link_libraries(session_node PRIVATE
  session_node_lib
  slotnexus::transport
  slotnexus::session
  slotnexus::rag
  slotnexus::fake_backends
  slotnexus::net_backends
  slotnexus::protocol
)
