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

# Fake 语音合成：确定性 PCM 块序列、取消语义与会话重置。
add_executable(fake_tts_test fake_tts_test.cpp)
target_link_libraries(fake_tts_test PRIVATE voxorchestra::fake_backends)
add_test(NAME fake_tts_test COMMAND fake_tts_test)

# Fake 音频输出（WAV）：文件头、数据往返与生命周期语义。
add_executable(fake_audio_sink_test fake_audio_sink_test.cpp)
target_link_libraries(fake_audio_sink_test PRIVATE voxorchestra::fake_backends)
add_test(NAME fake_audio_sink_test COMMAND fake_audio_sink_test)

# sherpa-onnx 真实识别（板端；仅硬件后端构建时编译）：
# 固定 WAV（test_wavs/0.wav）→ kPartial 渐进 + kFinal，最终文本与门禁
# 基线对齐。模型目录经 VOXORCHESTRA_ASR_MODEL 注入；未配置时跳过（返回 0）。
set(VOXORCHESTRA_ASR_MODEL ""
    CACHE PATH "sherpa-onnx ASR 单元测试用模型目录（含 test_wavs/0.wav）")
if(VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS)
  add_executable(sherpa_asr_test sherpa_asr_test.cpp)
  target_link_libraries(sherpa_asr_test PRIVATE
    voxorchestra::sherpa_asr voxorchestra::common)
  add_test(NAME sherpa_asr_test COMMAND sherpa_asr_test)
  set_tests_properties(sherpa_asr_test PROPERTIES
    ENVIRONMENT "VOXORCHESTRA_ASR_MODEL=${VOXORCHESTRA_ASR_MODEL}")
endif()

# RKLLM 真实生成（板端；仅硬件后端构建时编译）：
# 固定 prompt（与门禁 smoke 同款）→ kToken 流 + 末事件 kDone，结构断言
# + TTFT/tok/s 指标打印，输出与 smoke 对照记录进证据文档。
# 模型路径经 VOXORCHESTRA_RKLLM_MODEL 注入；未配置时测试跳过（返回 0）。
set(VOXORCHESTRA_RKLLM_MODEL ""
    CACHE PATH "RKLLM 单元测试用模型路径（.rkllm）")
if(VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS)
  add_executable(rkllm_llm_test rkllm_llm_test.cpp)
  target_link_libraries(rkllm_llm_test PRIVATE voxorchestra::rkllm)
  add_test(NAME rkllm_llm_test COMMAND rkllm_llm_test)
  set_tests_properties(rkllm_llm_test PROPERTIES
    ENVIRONMENT "VOXORCHESTRA_RKLLM_MODEL=${VOXORCHESTRA_RKLLM_MODEL}")
endif()

# SummerTTS 真实合成（板端；仅硬件后端构建时编译）：
# 固定文本 → kPcm 帧 + kDone，dataLen 与门禁基线（53248）对齐。
# 模型路径经 VOXORCHESTRA_TTS_MODEL 注入；未配置时测试跳过（返回 0）。
set(VOXORCHESTRA_TTS_MODEL ""
    CACHE PATH "SummerTTS 单元测试用模型路径（single_speaker_fast.bin）")
if(VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS)
  add_executable(summer_tts_test summer_tts_test.cpp)
  target_link_libraries(summer_tts_test PRIVATE voxorchestra::summer_tts)
  add_test(NAME summer_tts_test COMMAND summer_tts_test)
  set_tests_properties(summer_tts_test PROPERTIES
    ENVIRONMENT "VOXORCHESTRA_TTS_MODEL=${VOXORCHESTRA_TTS_MODEL}")
endif()

# ALSA 音频（板端真实声卡；仅硬件后端构建时编译）：
# sink 生命周期/drain 语义 + source 帧长/RMS。设备名经
# VOXORCHESTRA_ALSA_DEVICE 注入（未配置默认 "default"）；无模型 env，
# open 失败记录并跳过（板端无声卡时不能挂死）。
set(VOXORCHESTRA_ALSA_DEVICE ""
    CACHE STRING "ALSA 单元测试用设备名（默认 default）")
if(VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS)
  add_executable(alsa_audio_test alsa_audio_test.cpp)
  target_link_libraries(alsa_audio_test PRIVATE voxorchestra::alsa)
  add_test(NAME alsa_audio_test COMMAND alsa_audio_test)
  set_tests_properties(alsa_audio_test PROPERTIES
    ENVIRONMENT "VOXORCHESTRA_ALSA_DEVICE=${VOXORCHESTRA_ALSA_DEVICE}")
endif()

# 轻量 RAG：文本规范化、BM25 手算对照、JSONL 知识库与 L0-L3 路由。
add_executable(rag_test rag_test.cpp)
target_link_libraries(rag_test PRIVATE voxorchestra::rag)
add_test(NAME rag_test COMMAND rag_test)

# RAG 固定测试集：21 条查询逐条断言路由级别（知识库与阈值配套冻结）。
add_executable(rag_testset_test rag_testset_test.cpp)
target_link_libraries(rag_testset_test PRIVATE voxorchestra::rag)
add_test(NAME rag_testset_test COMMAND rag_testset_test)

# 有界队列：容量、满队列行为（拒绝/超时/阻塞）、关闭、清空、峰值与并发。
add_executable(bounded_queue_test bounded_queue_test.cpp)
target_link_libraries(bounded_queue_test PRIVATE voxorchestra::common)
add_test(NAME bounded_queue_test COMMAND bounded_queue_test)

# 分句器：中英文句末标点、连续标点、换行与流式切分。
add_executable(sentence_chunker_test sentence_chunker_test.cpp)
target_link_libraries(sentence_chunker_test PRIVATE voxorchestra::common)
add_test(NAME sentence_chunker_test COMMAND sentence_chunker_test)

# WAV 读取器：16-bit PCM 解析、未知块跳过、格式拒绝与损坏输入。
add_executable(wav_reader_test wav_reader_test.cpp)
target_link_libraries(wav_reader_test PRIVATE voxorchestra::common)
add_test(NAME wav_reader_test COMMAND wav_reader_test)

# Session 状态机：合法/非法迁移、任意阶段取消与轨迹记录。
add_executable(session_state_test session_state_test.cpp)
target_link_libraries(session_state_test PRIVATE voxorchestra::session)
add_test(NAME session_state_test COMMAND session_state_test)

# Session 编排管线：四类路由、WAV 输入、满队列丢弃、取消传播与晚到过滤。
add_executable(session_pipeline_test session_pipeline_test.cpp)
target_link_libraries(session_pipeline_test PRIVATE
  voxorchestra::session
  voxorchestra::fake_backends
)
add_test(NAME session_pipeline_test COMMAND session_pipeline_test)
