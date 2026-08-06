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

# 数据面全链路：session_node --backend net + 三节点（fake + 事件出口）。
# spawn 的节点二进制须随测试目标一起构建（add_test 不构建子进程可执行）。
add_executable(session_net_e2e_test session_net_e2e_test.cpp)
target_link_libraries(session_net_e2e_test PRIVATE
  voxorchestra::backend
  voxorchestra::common
  voxorchestra::protocol
  voxorchestra::transport
)
add_dependencies(session_net_e2e_test
  session_node asr_node llm_node tts_node
)
add_test(NAME session_net_e2e_test COMMAND session_net_e2e_test)
