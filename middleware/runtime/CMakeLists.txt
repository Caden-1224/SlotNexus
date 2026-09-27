# 通用节点运行时：JSON IBackend 契约、任务状态机与任务表。
#
# Author: Caden
add_library(slotnexus_runtime STATIC
  src/task_channel.cpp
  src/task_runtime.cpp
)
add_library(slotnexus::runtime ALIAS slotnexus_runtime)
set_target_properties(slotnexus_runtime PROPERTIES EXPORT_NAME runtime)
target_include_directories(slotnexus_runtime PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(slotnexus_runtime PUBLIC
  slotnexus::task_registry
  slotnexus::json)
target_compile_features(slotnexus_runtime PUBLIC cxx_std_17)
install(TARGETS slotnexus_runtime EXPORT slotnexus-core-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
