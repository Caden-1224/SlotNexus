# 任务注册表：work_id 的分配、查询与释放。
add_library(slotnexus_task_registry STATIC
  src/task_registry.cpp
)
add_library(slotnexus::task_registry ALIAS slotnexus_task_registry)

target_include_directories(slotnexus_task_registry PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(slotnexus_task_registry PUBLIC slotnexus::common)
target_compile_features(slotnexus_task_registry PUBLIC cxx_std_17)
