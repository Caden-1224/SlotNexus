# Fake 语音合成节点：RuntimeNode 外壳 + FakeTtsBackend + FakeAudioSink(WAV)。
add_executable(tts_node main.cpp)
target_link_libraries(tts_node PRIVATE voxorchestra::app_common voxorchestra::fake_backends)
