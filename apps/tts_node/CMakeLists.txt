# 语音合成节点：RuntimeNode 外壳 + ITtsBackend 工厂注入。
# 默认 FakeTtsBackend + FakeAudioSink(WAV)；硬件后端开启时接入 MeloTTS
# （板端默认真实合成）与 AlsaAudioSink（--sink alsa 板端声卡播放）。
# SummerTTS 仅在显式提供上游源码根目录时一并接入。
add_executable(tts_node main.cpp)
target_link_libraries(tts_node PRIVATE slotnexus::app_common slotnexus::fake_backends)
if(SLOTNEXUS_ENABLE_HARDWARE_BACKENDS)
  target_compile_definitions(tts_node PRIVATE SLOTNEXUS_HAS_MELOTTS=1)
  target_compile_definitions(tts_node PRIVATE SLOTNEXUS_HAS_ALSA=1)
  target_link_libraries(tts_node PRIVATE slotnexus::melotts slotnexus::alsa)
  if(TARGET slotnexus::summer_tts)
    target_compile_definitions(tts_node PRIVATE SLOTNEXUS_HAS_SUMMERTTS=1)
    target_link_libraries(tts_node PRIVATE slotnexus::summer_tts)
  endif()
endif()
