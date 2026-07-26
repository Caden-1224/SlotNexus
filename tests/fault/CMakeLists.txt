# 故障注入测试：Session 管线的异常后端、在途推理中 exit/cancel、忙时拒绝、
# 挂起后端 max_run 兜底超时、重复 cancel/exit（进程内）+ 链路级故障注入
# （非法 JSON、超长帧、未知 work_id、错误输入后进程可正常退出）。
add_executable(session_fault_test session_fault_test.cpp)
target_link_libraries(session_fault_test PRIVATE
  session_node_lib
  voxorchestra::transport
  voxorchestra::session
  voxorchestra::fake_backends
  voxorchestra::protocol
)
add_test(NAME session_fault_test COMMAND session_fault_test)

# 链路级：client --TCP--> gateway --ZMQ--> manager --ZMQ--> session_node 真实进程。
add_executable(chain_fault_test chain_fault_test.cpp)
target_link_libraries(chain_fault_test PRIVATE
  voxorchestra::protocol
)
add_test(NAME chain_fault_test COMMAND chain_fault_test)
