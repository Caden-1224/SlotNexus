# 语音合成节点：RuntimeNode 外壳 + ITtsBackend 工厂注入。
# 默认 FakeTtsBackend + FakeAudioSink(WAV)；硬件后端开启时接入 SummerTTS
# 与 AlsaAudioSink（--sink alsa 板端声卡播放）。
add_executable(tts_node main.cpp)
target_link_libraries(tts_node PRIVATE voxorchestra::app_common voxorchestra::fake_backends)
if(VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS)
  target_compile_definitions(tts_node PRIVATE VOXORCHESTRA_HAS_SUMMERTTS=1)
  target_compile_definitions(tts_node PRIVATE VOXORCHESTRA_HAS_ALSA=1)
  target_link_libraries(tts_node PRIVATE voxorchestra::summer_tts voxorchestra::alsa)
endif()
