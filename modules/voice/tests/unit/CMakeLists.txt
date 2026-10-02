# 语音单元测试。
#
# Author: Caden
function(vox_voice_unit_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE ${ARGN})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "${VOX_TEST_ENV}")
  set_target_properties(${name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/unit)
endfunction()

vox_voice_unit_test(backend_contract_test slotnexus::voice_backend)
vox_voice_unit_test(fake_asr_test slotnexus::voice_fake_backends)
vox_voice_unit_test(fake_llm_test slotnexus::voice_fake_backends)
vox_voice_unit_test(fake_tts_test slotnexus::voice_fake_backends)
vox_voice_unit_test(fake_audio_sink_test slotnexus::voice_fake_backends)
vox_voice_unit_test(rag_test slotnexus::voice_rag)
vox_voice_unit_test(rag_testset_test slotnexus::voice_rag)
vox_voice_unit_test(sentence_chunker_test slotnexus::voice_common)
vox_voice_unit_test(wav_reader_test slotnexus::voice_common)
vox_voice_unit_test(session_pipeline_test slotnexus::voice_session
  slotnexus::voice_fake_backends)
vox_voice_unit_test(streaming_input_test slotnexus::voice_session)
vox_voice_unit_test(continuous_input_test slotnexus::voice_session)

add_executable(rkllm_reasoning_filter_test rkllm_reasoning_filter_test.cpp)
target_include_directories(rkllm_reasoning_filter_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../backends/rkllm/include)
add_test(NAME rkllm_reasoning_filter_test COMMAND rkllm_reasoning_filter_test)
set_tests_properties(rkllm_reasoning_filter_test PROPERTIES ENVIRONMENT "${VOX_TEST_ENV}")
set_target_properties(rkllm_reasoning_filter_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/unit)

if(SLOTNEXUS_ENABLE_HARDWARE_BACKENDS)
  set(SLOTNEXUS_ASR_MODEL "" CACHE PATH "sherpa-onnx ASR 测试模型目录")
  set(SLOTNEXUS_ASR_PRECISION "fp32" CACHE STRING "sherpa-onnx ASR 测试模型精度（fp32/int8）")
  set(SLOTNEXUS_VOICE_FIXTURE_DIR "" CACHE PATH "demo_zh.wav 端到端固定 fixture 目录")
  set(SLOTNEXUS_RKLLM_MODEL "" CACHE PATH "RKLLM 测试模型路径")
  set(SLOTNEXUS_RKLLM_REASONING_END_TAG "off" CACHE STRING "RKLLM 思考段过滤标记")
  set(SLOTNEXUS_MELOTTS_ENCODER "" CACHE PATH "MeloTTS 编码器 onnx")
  set(SLOTNEXUS_MELOTTS_DECODER "" CACHE PATH "MeloTTS 解码器 rknn")
  set(SLOTNEXUS_MELOTTS_LEXICON "" CACHE PATH "MeloTTS lexicon.txt")
  set(SLOTNEXUS_MELOTTS_TOKENS "" CACHE PATH "MeloTTS tokens.txt")
  set(SLOTNEXUS_MELOTTS_G "" CACHE PATH "MeloTTS g 向量")
  set(SLOTNEXUS_ALSA_DEVICE "" CACHE STRING "ALSA 测试设备名")

  add_executable(sherpa_asr_test sherpa_asr_test.cpp)
  target_link_libraries(sherpa_asr_test PRIVATE
    slotnexus::sherpa_asr slotnexus::voice_common)
  add_test(NAME sherpa_asr_test COMMAND sherpa_asr_test)
  set_tests_properties(sherpa_asr_test PROPERTIES
    ENVIRONMENT "SLOTNEXUS_ASR_MODEL=${SLOTNEXUS_ASR_MODEL};SLOTNEXUS_ASR_PRECISION=${SLOTNEXUS_ASR_PRECISION};SLOTNEXUS_VOICE_FIXTURE_DIR=${SLOTNEXUS_VOICE_FIXTURE_DIR}")

  add_executable(streaming_input_hardware_test streaming_input_hardware_test.cpp)
  target_link_libraries(streaming_input_hardware_test PRIVATE
    slotnexus::sherpa_asr slotnexus::voice_session slotnexus::voice_common)
  add_test(NAME streaming_input_hardware_test COMMAND streaming_input_hardware_test)
  set_tests_properties(streaming_input_hardware_test PROPERTIES
    ENVIRONMENT "SLOTNEXUS_ASR_MODEL=${SLOTNEXUS_ASR_MODEL};SLOTNEXUS_ASR_PRECISION=${SLOTNEXUS_ASR_PRECISION};SLOTNEXUS_VOICE_FIXTURE_DIR=${SLOTNEXUS_VOICE_FIXTURE_DIR}")

  add_executable(rkllm_llm_test rkllm_llm_test.cpp)
  target_link_libraries(rkllm_llm_test PRIVATE slotnexus::rkllm)
  add_test(NAME rkllm_llm_test COMMAND rkllm_llm_test)
  set_tests_properties(rkllm_llm_test PROPERTIES
    ENVIRONMENT "SLOTNEXUS_RKLLM_MODEL=${SLOTNEXUS_RKLLM_MODEL};SLOTNEXUS_RKLLM_REASONING_END_TAG=${SLOTNEXUS_RKLLM_REASONING_END_TAG}")

  add_executable(melotts_tts_test melotts_tts_test.cpp)
  target_link_libraries(melotts_tts_test PRIVATE slotnexus::melotts)
  add_test(NAME melotts_tts_test COMMAND melotts_tts_test)
  set_tests_properties(melotts_tts_test PROPERTIES
    ENVIRONMENT "SLOTNEXUS_MELOTTS_ENCODER=${SLOTNEXUS_MELOTTS_ENCODER};SLOTNEXUS_MELOTTS_DECODER=${SLOTNEXUS_MELOTTS_DECODER};SLOTNEXUS_MELOTTS_LEXICON=${SLOTNEXUS_MELOTTS_LEXICON};SLOTNEXUS_MELOTTS_TOKENS=${SLOTNEXUS_MELOTTS_TOKENS};SLOTNEXUS_MELOTTS_G=${SLOTNEXUS_MELOTTS_G}")

  add_executable(melotts_pcm_converter_test melotts_pcm_converter_test.cpp)
  target_link_libraries(melotts_pcm_converter_test PRIVATE slotnexus::melotts)
  target_include_directories(melotts_pcm_converter_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../backends/melotts/src)
  add_test(NAME melotts_pcm_converter_test COMMAND melotts_pcm_converter_test)
  set_tests_properties(melotts_pcm_converter_test PROPERTIES
    ENVIRONMENT "${VOX_TEST_ENV}")

  add_executable(alsa_audio_test alsa_audio_test.cpp)
  target_link_libraries(alsa_audio_test PRIVATE slotnexus::alsa)
  add_test(NAME alsa_audio_test COMMAND alsa_audio_test)
  set_tests_properties(alsa_audio_test PROPERTIES
    ENVIRONMENT "SLOTNEXUS_ALSA_DEVICE=${SLOTNEXUS_ALSA_DEVICE}")

  foreach(target_name IN ITEMS sherpa_asr_test streaming_input_hardware_test rkllm_llm_test melotts_tts_test melotts_pcm_converter_test alsa_audio_test)
    set_target_properties(${target_name} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/unit)
  endforeach()
endif()
