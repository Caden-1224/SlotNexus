# 通用 TCP 接入网关。
#
# Author: Caden
add_library(slotnexus_edge_gateway STATIC
  edge_gateway.cpp
)
add_library(slotnexus::edge_gateway ALIAS slotnexus_edge_gateway)
set_target_properties(slotnexus_edge_gateway PROPERTIES EXPORT_NAME edge_gateway)
target_include_directories(slotnexus_edge_gateway PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/slotnexus/services>)
target_link_libraries(slotnexus_edge_gateway PUBLIC
  slotnexus::network
  slotnexus::transport
  slotnexus::protocol)
target_compile_features(slotnexus_edge_gateway PUBLIC cxx_std_17)
install(TARGETS slotnexus_edge_gateway EXPORT slotnexus-core-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES edge_gateway.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/slotnexus/services)

add_executable(edge_gateway main.cpp)
target_link_libraries(edge_gateway PRIVATE slotnexus::edge_gateway)
set_target_properties(edge_gateway PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/edge_gateway)
install(TARGETS edge_gateway RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
