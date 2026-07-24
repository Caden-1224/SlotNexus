# 端到端测试：Session 全链路（gateway → manager → session_node → WAV 输出）。
add_executable(session_e2e_test session_e2e_test.cpp)
target_link_libraries(session_e2e_test PRIVATE
  voxorchestra::protocol
  voxorchestra::transport
)
add_test(NAME session_e2e_test COMMAND session_e2e_test)
