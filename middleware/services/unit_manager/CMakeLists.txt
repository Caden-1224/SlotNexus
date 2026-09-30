# 通用 Unit Manager 库与进程入口。
#
# Author: Caden
add_library(slotnexus_unit_manager STATIC
  unit_manager.cpp
)
add_library(slotnexus::unit_manager ALIAS slotnexus_unit_manager)
set_target_properties(slotnexus_unit_manager PROPERTIES EXPORT_NAME unit_manager)
target_include_directories(slotnexus_unit_manager PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/slotnexus/services>)
target_link_libraries(slotnexus_unit_manager PUBLIC
  slotnexus::node_host
  slotnexus::task_registry
  slotnexus::transport)
target_compile_features(slotnexus_unit_manager PUBLIC cxx_std_17)
install(TARGETS slotnexus_unit_manager EXPORT slotnexus-core-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ../include/slotnexus/services/unit_manager.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/slotnexus/services)

add_executable(unit_manager main.cpp)
target_link_libraries(unit_manager PRIVATE slotnexus::unit_manager)
set_target_properties(unit_manager PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/unit_manager)
install(TARGETS unit_manager RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
