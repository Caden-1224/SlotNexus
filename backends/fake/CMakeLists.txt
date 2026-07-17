# 确定性 Fake Backend 实现（默认构建）。
#
# 只依赖 voxorchestra::backend 契约，不依赖任何厂商 SDK：
# FakeAsrBackend / FakeRetriever / FakeLlmBackend / FakeTtsBackend /
# FakeAudioSink（WAV 写出）逐步接入。
add_library(voxorchestra_fake_backends INTERFACE)
add_library(voxorchestra::fake_backends ALIAS voxorchestra_fake_backends)

target_link_libraries(voxorchestra_fake_backends INTERFACE voxorchestra::backend)
target_include_directories(voxorchestra_fake_backends INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
