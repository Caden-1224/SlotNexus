# 网络库：NDJSON 解帧、epoll Reactor 与 TCP Gateway（逐步扩充）
add_library(voxorchestra_network STATIC
  src/frame_decoder.cpp
  src/channel.cpp
  src/poller.cpp
  src/event_loop.cpp
  src/tcp_connection.cpp
  src/tcp_server.cpp
)
add_library(voxorchestra::network ALIAS voxorchestra_network)

target_include_directories(voxorchestra_network PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(voxorchestra_network PUBLIC voxorchestra::common)
target_compile_features(voxorchestra_network PUBLIC cxx_std_17)
