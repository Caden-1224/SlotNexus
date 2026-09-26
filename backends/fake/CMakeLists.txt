# 确定性 Fake Backend 实现（默认构建）。
#
# 只依赖 slotnexus::backend 契约，不依赖任何厂商 SDK：
# FakeAsrBackend / FakeRetriever / FakeLlmBackend / FakeTtsBackend /
# FakeAudioSink（WAV 写出）逐步接入。
add_library(slotnexus_fake_backends INTERFACE)
add_library(slotnexus::fake_backends ALIAS slotnexus_fake_backends)

target_link_libraries(slotnexus_fake_backends INTERFACE slotnexus::backend)
target_include_directories(slotnexus_fake_backends INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
