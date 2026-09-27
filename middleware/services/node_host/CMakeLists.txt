# 通用节点外壳库。
#
# Author: Caden
add_library(slotnexus_node_host STATIC
  runtime_node.cpp
)
add_library(slotnexus::node_host ALIAS slotnexus_node_host)
set_target_properties(slotnexus_node_host PROPERTIES EXPORT_NAME node_host)
target_include_directories(slotnexus_node_host PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/slotnexus/services>)
target_link_libraries(slotnexus_node_host PUBLIC
  slotnexus::transport
  slotnexus::protocol
  slotnexus::runtime
  slotnexus::dataplane)
target_compile_features(slotnexus_node_host PUBLIC cxx_std_17)
install(TARGETS slotnexus_node_host EXPORT slotnexus-core-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES runtime_node.hpp action_helpers.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/slotnexus/services)
