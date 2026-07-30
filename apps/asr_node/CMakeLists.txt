# 语音识别节点：RuntimeNode 外壳 + IAsrBackend 工厂注入。
# 默认 FakeAsrBackend（Mock 帧数负载）；硬件后端开启时接入 sherpa-onnx。
add_executable(asr_node main.cpp)
target_link_libraries(asr_node PRIVATE
  voxorchestra::app_common voxorchestra::fake_backends voxorchestra::common)
if(VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS)
  target_compile_definitions(asr_node PRIVATE VOXORCHESTRA_HAS_SHERTA_ONNX=1)
  target_link_libraries(asr_node PRIVATE voxorchestra::sherpa_asr)
endif()
