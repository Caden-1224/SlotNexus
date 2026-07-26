# 端到端测试：Session 全链路（gateway → manager → session_node → WAV 输出）。
add_executable(session_e2e_test session_e2e_test.cpp)
target_link_libraries(session_e2e_test PRIVATE
  voxorchestra::protocol
  voxorchestra::transport
)
add_test(NAME session_e2e_test COMMAND session_e2e_test)

# 50 轮轮换回归（M1 门禁 1/2/3）：四类路由+WAV 轮换、零跨流、
# 进程/端口残留与日志 request_id 关联。
add_executable(session_rounds_test session_rounds_test.cpp)
target_link_libraries(session_rounds_test PRIVATE
  voxorchestra::protocol
  voxorchestra::transport
)
add_test(NAME session_rounds_test COMMAND session_rounds_test)
