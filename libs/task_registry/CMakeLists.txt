# 任务注册表：work_id 的分配、查询与释放。
add_library(voxorchestra_task_registry STATIC
  src/task_registry.cpp
)
add_library(voxorchestra::task_registry ALIAS voxorchestra_task_registry)

target_include_directories(voxorchestra_task_registry PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(voxorchestra_task_registry PUBLIC voxorchestra::common)
target_compile_features(voxorchestra_task_registry PUBLIC cxx_std_17)
