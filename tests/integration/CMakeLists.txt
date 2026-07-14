# 集成测试：三种 ZMQ 模式的真实收发（inproc 端点），验证超时、关闭与恢复。
add_executable(rpc_integration_test rpc_integration_test.cpp)
target_link_libraries(rpc_integration_test PRIVATE voxorchestra::transport)
add_test(NAME rpc_integration_test COMMAND rpc_integration_test)

add_executable(pubsub_integration_test pubsub_integration_test.cpp)
target_link_libraries(pubsub_integration_test PRIVATE voxorchestra::transport)
add_test(NAME pubsub_integration_test COMMAND pubsub_integration_test)

add_executable(pushpull_integration_test pushpull_integration_test.cpp)
target_link_libraries(pushpull_integration_test PRIVATE voxorchestra::transport)
add_test(NAME pushpull_integration_test COMMAND pushpull_integration_test)

add_executable(tcp_server_test tcp_server_test.cpp)
target_link_libraries(tcp_server_test PRIVATE voxorchestra::network)
add_test(NAME tcp_server_test COMMAND tcp_server_test)

add_executable(gateway_test gateway_test.cpp)
target_link_libraries(gateway_test PRIVATE edge_gateway_lib)
add_test(NAME gateway_test COMMAND gateway_test)
