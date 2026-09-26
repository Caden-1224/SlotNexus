# 通用基础库：版本、有界队列、分句器、WAV 读取等通用工具
add_library(slotnexus_common STATIC
  src/version.cpp
  src/sentence_chunker.cpp
  src/wav_reader.cpp
)
add_library(slotnexus::common ALIAS slotnexus_common)

target_include_directories(slotnexus_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(slotnexus_common PUBLIC cxx_std_17)
