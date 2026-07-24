# 故障注入测试：Session 管线的异常后端、在途推理中 exit/cancel、忙时拒绝。
add_executable(session_fault_test session_fault_test.cpp)
target_link_libraries(session_fault_test PRIVATE
  session_node_lib
  voxorchestra::transport
  voxorchestra::session
  voxorchestra::fake_backends
  voxorchestra::protocol
)
add_test(NAME session_fault_test COMMAND session_fault_test)
