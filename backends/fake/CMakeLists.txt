# 确定性 Fake Backend 实现（默认构建）。
#
# 只依赖 voxorchestra::backend 契约，不依赖任何厂商 SDK：
# FakeAsrBackend / FakeRetriever / FakeLlmBackend / FakeTtsBackend /
# FakeAudioSink（WAV 写出）逐步接入。
