# 外部客户端：TCP NDJSON → Gateway。
#
# Author: Caden
add_executable(voice_cli main.cpp)
target_link_libraries(voice_cli PRIVATE slotnexus::protocol)
set_target_properties(voice_cli PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/voice_cli)
