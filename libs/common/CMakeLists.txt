# 通用基础库：版本、错误码、通用工具（逐步扩充）
add_library(voxorchestra_common STATIC
  src/version.cpp
)
add_library(voxorchestra::common ALIAS voxorchestra_common)

target_include_directories(voxorchestra_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(voxorchestra_common PUBLIC cxx_std_17)
