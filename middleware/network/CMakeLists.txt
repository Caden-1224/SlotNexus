# 网络库：NDJSON 解帧、epoll Reactor 与 TCP Gateway 基础设施。
#
# Author: Caden
add_library(slotnexus_network STATIC
  src/frame_decoder.cpp
  src/channel.cpp
  src/poller.cpp
  src/event_loop.cpp
  src/tcp_connection.cpp
  src/tcp_server.cpp
)
add_library(slotnexus::network ALIAS slotnexus_network)
set_target_properties(slotnexus_network PROPERTIES EXPORT_NAME network)
target_include_directories(slotnexus_network PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(slotnexus_network PUBLIC slotnexus::common)
target_compile_features(slotnexus_network PUBLIC cxx_std_17)
install(TARGETS slotnexus_network EXPORT slotnexus-core-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
