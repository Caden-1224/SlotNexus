# 任务注册表：work_id 的分配、查询与释放。
#
# Author: Caden
add_library(slotnexus_task_registry STATIC
  src/task_registry.cpp
)
add_library(slotnexus::task_registry ALIAS slotnexus_task_registry)
set_target_properties(slotnexus_task_registry PROPERTIES EXPORT_NAME task_registry)
target_include_directories(slotnexus_task_registry PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(slotnexus_task_registry PUBLIC slotnexus::common)
target_compile_features(slotnexus_task_registry PUBLIC cxx_std_17)
install(TARGETS slotnexus_task_registry EXPORT slotnexus-core-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
