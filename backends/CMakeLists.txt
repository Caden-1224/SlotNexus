# 可替换推理后端目录。
#
# 契约库（纯头文件接口，无任何第三方依赖）默认构建；Fake 模拟实现无条件接入，
# 保证 x86 开发环境不需要厂商 SDK 或声卡。硬件后端（sherpa_onnx / rkllm /
# summer_tts / alsa）只能在 VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=ON 时构建。

# Backend 契约：IAsrBackend / IRetriever / ILlmBackend / ITtsBackend / IAudioSink
# 与统一事件 BackendEvent。接口为纯虚类，无实现文件。
add_library(voxorchestra_backend INTERFACE)
add_library(voxorchestra::backend ALIAS voxorchestra_backend)

target_include_directories(voxorchestra_backend INTERFACE
 ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(voxorchestra_backend INTERFACE cxx_std_17)

# Fake 模拟实现（确定性输出，用于测试协议与编排，默认构建）。
add_subdirectory(fake)

if(VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS)
 # 硬件后端按天接入：sherpa_onnx/ rkllm/
 # summer_tts（，已接入）/ alsa。
 add_subdirectory(sherpa_onnx)
 add_subdirectory(rkllm)
 add_subdirectory(summer_tts)
endif()
