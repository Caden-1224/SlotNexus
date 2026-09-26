# 控制面共享类：RuntimeNode（节点服务）与 UnitManager（任务生命周期/路由）。
add_library(slotnexus_app_common STATIC
  runtime_node.cpp
  unit_manager.cpp
)
add_library(slotnexus::app_common ALIAS slotnexus_app_common)

target_include_directories(slotnexus_app_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(slotnexus_app_common PUBLIC
  slotnexus::transport slotnexus::protocol slotnexus::runtime
  slotnexus::task_registry slotnexus::dataplane
)
target_compile_features(slotnexus_app_common PUBLIC cxx_std_17)
