# 网关核心逻辑做成静态库，供可执行程序与集成测试共用。
add_library(edge_gateway_lib STATIC
  edge_gateway.cpp
)
target_include_directories(edge_gateway_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(edge_gateway_lib PUBLIC voxorchestra::network voxorchestra::protocol voxorchestra::transport)

add_executable(edge_gateway main.cpp)
target_link_libraries(edge_gateway PRIVATE edge_gateway_lib)
