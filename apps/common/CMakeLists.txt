# 控制面共享类：RuntimeNode（节点服务）与 UnitManager（任务生命周期/路由）。
add_library(voxorchestra_app_common STATIC
  runtime_node.cpp
  unit_manager.cpp
)
add_library(voxorchestra::app_common ALIAS voxorchestra_app_common)

target_include_directories(voxorchestra_app_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(voxorchestra_app_common PUBLIC
  voxorchestra::transport voxorchestra::protocol voxorchestra::runtime
  voxorchestra::task_registry voxorchestra::dataplane
)
target_compile_features(voxorchestra_app_common PUBLIC cxx_std_17)
