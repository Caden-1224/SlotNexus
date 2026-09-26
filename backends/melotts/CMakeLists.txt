# MeloTTS 后端（板端 aarch64；仅 SLOTNEXUS_ENABLE_HARDWARE_BACKENDS=ON 时构建）。
#
# 上游源码与模型不随仓库分发（third_party/README.md 约定）。构建时由
# SLOTNEXUS_MELOTTS_ROOT 指向板端依赖根，目录约定：
#   include/onnxruntime_c_api.h   include/rknn_api.h
#   lib/libonnxruntime.so         lib/librknnrt.so
# 模型与文本资源（encoder-zh.onnx / decoder-zh.rknn / lexicon.txt / tokens.txt /
# g-zh_mix_en.bin）路径经 config/taishanpi3m/session.json::tts 传入。
#
# 与 SummerTTS 后端的关系：本后端是当前板端默认 TTS 真实后端（中文与中英混读，
# ONNX Runtime CPU 编码器 + RKNN NPU 解码器）；SummerTTS 后端保留但只在显式
# 提供 SLOTNEXUS_SUMMERTTS_ROOT 时才参与构建。
set(SLOTNEXUS_MELOTTS_ROOT ""
    CACHE PATH "MeloTTS 依赖根目录（含 ONNX Runtime 与 RKNN 的 include/ 与 lib/）")
if(NOT SLOTNEXUS_MELOTTS_ROOT)
  message(FATAL_ERROR
    "SLOTNEXUS_ENABLE_HARDWARE_BACKENDS=ON 需要 "
    "-DSLOTNEXUS_MELOTTS_ROOT=<ONNX Runtime + RKNN 依赖根目录>")
endif()

foreach(_melotts_required
    "${SLOTNEXUS_MELOTTS_ROOT}/include/onnxruntime_c_api.h"
    "${SLOTNEXUS_MELOTTS_ROOT}/include/rknn_api.h"
    "${SLOTNEXUS_MELOTTS_ROOT}/lib/libonnxruntime.so"
    "${SLOTNEXUS_MELOTTS_ROOT}/lib/librknnrt.so")
  if(NOT EXISTS "${_melotts_required}")
    message(FATAL_ERROR "MeloTTS 依赖缺失: ${_melotts_required}")
  endif()
endforeach()

add_library(slotnexus_melotts STATIC
  src/melotts_text_frontend.cpp
  src/melotts_pcm_converter.cpp
  src/melotts_real_engines.cpp
  src/melotts_tts_backend.cpp
)
add_library(slotnexus::melotts ALIAS slotnexus_melotts)

# 契约头（backend_event.hpp / i_tts_backend.hpp）是本库的公共 API，必须向消费者
# 传播（melotts_tts_test / tts_node 都要 include）；ONNX Runtime 与 RKNN 仅内部
# 使用，保持 PRIVATE（静态库的 PRIVATE 链接依赖由 CMake 以 $<LINK_ONLY> 传播给
# 消费者，链接时仍会带上）。
target_link_libraries(slotnexus_melotts PUBLIC slotnexus::backend)
target_link_libraries(slotnexus_melotts PRIVATE
  "${SLOTNEXUS_MELOTTS_ROOT}/lib/libonnxruntime.so"
  "${SLOTNEXUS_MELOTTS_ROOT}/lib/librknnrt.so"
)
target_include_directories(slotnexus_melotts PRIVATE
  "${SLOTNEXUS_MELOTTS_ROOT}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_include_directories(slotnexus_melotts PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_options(slotnexus_melotts PRIVATE -O3)
target_compile_features(slotnexus_melotts PUBLIC cxx_std_17)
target_compile_definitions(slotnexus_melotts PUBLIC SLOTNEXUS_HAS_MELOTTS=1)
