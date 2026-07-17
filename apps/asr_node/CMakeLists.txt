# Fake 语音识别节点：RuntimeNode 外壳 + FakeAsrBackend 适配器。
add_executable(asr_node main.cpp)
target_link_libraries(asr_node PRIVATE voxorchestra::app_common voxorchestra::fake_backends)
