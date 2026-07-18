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
target_link_libraries(gateway_test PRIVATE edge_gateway_lib voxorchestra::app_common)
add_test(NAME gateway_test COMMAND gateway_test)

# 端到端验收：真实三进程拓扑的 Echo 链路。
add_executable(echo_e2e_test echo_e2e_test.cpp)
target_link_libraries(echo_e2e_test PRIVATE voxorchestra::protocol)
add_test(NAME echo_e2e_test COMMAND echo_e2e_test)

# 端到端验收：五节点（echo + 四类 Fake）单 Manager 轮转路由。
add_executable(fake_nodes_e2e_test fake_nodes_e2e_test.cpp)
target_link_libraries(fake_nodes_e2e_test PRIVATE voxorchestra::protocol)
add_test(NAME fake_nodes_e2e_test COMMAND fake_nodes_e2e_test)
