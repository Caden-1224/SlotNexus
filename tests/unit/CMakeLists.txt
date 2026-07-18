# 单元测试：验证最小库能正确编译、链接并通过 CTest 运行。
add_executable(smoke_test smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE voxorchestra::common)
add_test(NAME smoke_test COMMAND smoke_test)

# 协议基线：MessageEnvelope 编解码与校验。
add_executable(protocol_test protocol_test.cpp)
target_link_libraries(protocol_test PRIVATE voxorchestra::protocol)
add_test(NAME protocol_test COMMAND protocol_test)

# NDJSON 增量解帧。
add_executable(frame_decoder_test frame_decoder_test.cpp)
target_link_libraries(frame_decoder_test PRIVATE voxorchestra::network)
add_test(NAME frame_decoder_test COMMAND frame_decoder_test)

# epoll Reactor（EventLoop/Channel/Poller）。
add_executable(reactor_test reactor_test.cpp)
target_link_libraries(reactor_test PRIVATE voxorchestra::network)
add_test(NAME reactor_test COMMAND reactor_test)

# 任务注册表：work_id 生命周期与幂等。
add_executable(task_registry_test task_registry_test.cpp)
target_link_libraries(task_registry_test PRIVATE voxorchestra::task_registry)
add_test(NAME task_registry_test COMMAND task_registry_test)

# 节点运行时：任务状态机、隔离、超时、取消、重复 exit。
add_executable(runtime_test runtime_test.cpp)
target_link_libraries(runtime_test PRIVATE voxorchestra::runtime)
add_test(NAME runtime_test COMMAND runtime_test)

# Backend 契约：统一事件 BackendEvent 与音频常量。
add_executable(backend_contract_test backend_contract_test.cpp)
target_link_libraries(backend_contract_test PRIVATE voxorchestra::backend)
add_test(NAME backend_contract_test COMMAND backend_contract_test)

# Fake 语音识别：确定性 partial/final、取消语义与会话重置。
add_executable(fake_asr_test fake_asr_test.cpp)
target_link_libraries(fake_asr_test PRIVATE voxorchestra::fake_backends)
add_test(NAME fake_asr_test COMMAND fake_asr_test)

# Fake 文本检索：Top-K 语义、排序确定性与注入知识库。
add_executable(fake_retriever_test fake_retriever_test.cpp)
target_link_libraries(fake_retriever_test PRIVATE voxorchestra::fake_backends)
add_test(NAME fake_retriever_test COMMAND fake_retriever_test)

# Fake 大模型生成：token 序列、最终文本、取消语义与会话重置。
add_executable(fake_llm_test fake_llm_test.cpp)
target_link_libraries(fake_llm_test PRIVATE voxorchestra::fake_backends)
add_test(NAME fake_llm_test COMMAND fake_llm_test)
