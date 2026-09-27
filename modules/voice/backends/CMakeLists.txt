# 语音专有后端构建入口。
#
# Author: Caden
# 边界：所有 Backend 契约、Fake/网络/厂商实现都属于 voice 模块。
add_subdirectory(contract)
add_subdirectory(fake)
add_subdirectory(net)
if(SLOTNEXUS_ENABLE_HARDWARE_BACKENDS)
  add_subdirectory(sherpa_onnx)
  add_subdirectory(rkllm)
  add_subdirectory(melotts)
  add_subdirectory(alsa)
endif()
