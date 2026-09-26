# 数据面网络后端：把远端节点（asr/llm/tts）当作本地流式后端。
# 上行控制面 RPC（REQ/REP，setup/inference/cancel），下行数据面事件
# PUB/SUB 订阅回放。会话侧（session_node --backend net）依赖本库。
add_library(slotnexus_net_backends STATIC
  src/net_backend_session.cpp
)
add_library(slotnexus::net_backends ALIAS slotnexus_net_backends)

target_include_directories(slotnexus_net_backends PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(slotnexus_net_backends PUBLIC
  slotnexus::backend
  slotnexus::common
  slotnexus::dataplane
  slotnexus::transport
  slotnexus::protocol
)
target_compile_features(slotnexus_net_backends PUBLIC cxx_std_17)
