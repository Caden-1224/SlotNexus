# 通用 Echo 示例节点。
#
# Author: Caden
add_executable(echo_node main.cpp)
target_link_libraries(echo_node PRIVATE slotnexus::node_host)
set_target_properties(echo_node PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/echo_node)
install(TARGETS echo_node RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
