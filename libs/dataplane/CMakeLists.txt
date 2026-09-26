# 数据面库：流式事件协议（DataplaneEvent + PUB/SUB 事件通道），
# 节点侧发布流式后端中间事件（partial/token/PCM），会话侧订阅消费。
add_library(slotnexus_dataplane STATIC
  src/dataplane_event.cpp
  src/event_channel.cpp
)
add_library(slotnexus::dataplane ALIAS slotnexus_dataplane)

target_include_directories(slotnexus_dataplane PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# slotnexus::backend 为纯头契约库（BackendEvent），无条件构建。
target_link_libraries(slotnexus_dataplane PUBLIC
  slotnexus::transport
  slotnexus::protocol
  slotnexus::backend
)
target_compile_features(slotnexus_dataplane PUBLIC cxx_std_17)
