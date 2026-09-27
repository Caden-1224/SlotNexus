# 通用数据面：不透明 kind + JSON payload 事件通道。
#
# Author: Caden
add_library(slotnexus_dataplane STATIC
  src/dataplane_event.cpp
  src/event_channel.cpp
)
add_library(slotnexus::dataplane ALIAS slotnexus_dataplane)
set_target_properties(slotnexus_dataplane PROPERTIES EXPORT_NAME dataplane)
target_include_directories(slotnexus_dataplane PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(slotnexus_dataplane PUBLIC
  slotnexus::transport
  slotnexus::protocol
  slotnexus::json)
target_compile_features(slotnexus_dataplane PUBLIC cxx_std_17)
install(TARGETS slotnexus_dataplane EXPORT slotnexus-core-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
