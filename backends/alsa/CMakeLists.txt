# ALSA 音频后端（板端 aarch64；仅 VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=ON
# 时构建）。
#
# 与厂商 SDK 后端（sherpa_onnx / rkllm / summer_tts 的 VOXORCHESTRA_*_ROOT
# 门控）不同：ALSA 是系统包（libasound2-dev，板端 Ubuntu 24.04 =
# 1.2.11-1ubuntu0.3），无 ROOT 变量，直接 find_package(ALSA REQUIRED)。
# x86 开发机不必装 libasound2-dev：本目录只在硬件门控打开时 add_subdirectory。
#
# 契约头（i_audio_sink.hpp）是本库的公共 API，必须向消费者传播
# （alsa_audio_test / tts_node 都要 include）；ALSA::ALSA 仅内部使用，
# 保持 PRIVATE（静态库的 PRIVATE 链接依赖由 CMake 以 $<LINK_ONLY> 传播
# 给消费者，tts_node 链接时仍会带上 -lasound）。
find_package(ALSA REQUIRED)

add_library(voxorchestra_alsa STATIC
  src/alsa_audio_sink.cpp
  src/alsa_audio_source.cpp
)
add_library(voxorchestra::alsa ALIAS voxorchestra_alsa)

target_link_libraries(voxorchestra_alsa PUBLIC voxorchestra::backend)
target_link_libraries(voxorchestra_alsa PRIVATE ALSA::ALSA)
target_include_directories(voxorchestra_alsa PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(voxorchestra_alsa PUBLIC cxx_std_17)
