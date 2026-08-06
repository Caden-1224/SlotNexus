# 会话编排节点：Fake 后端 + 真实 BM25 路由 + 有界队列 + generation 取消。
# 依赖 voxorchestra::transport 引入 libzmq（ROUTER 服务端）。
add_library(session_node_lib STATIC
  session_node.cpp
)
target_include_directories(session_node_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(session_node_lib PRIVATE
  voxorchestra::transport
  voxorchestra::session
  voxorchestra::rag
  voxorchestra::fake_backends
  voxorchestra::net_backends
  voxorchestra::protocol
)

add_executable(session_node main.cpp)
target_link_libraries(session_node PRIVATE
  session_node_lib
  voxorchestra::transport
  voxorchestra::session
  voxorchestra::rag
  voxorchestra::fake_backends
  voxorchestra::net_backends
  voxorchestra::protocol
)
