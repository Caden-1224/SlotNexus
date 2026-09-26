# 协议库：统一消息 MessageEnvelope 的 JSON 编码/解码与校验
add_library(slotnexus_protocol STATIC
  src/message_envelope.cpp
)
add_library(slotnexus::protocol ALIAS slotnexus_protocol)

target_include_directories(slotnexus_protocol PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# nlohmann/json：优先使用包管理器提供的 cmake 配置，找不到时退回仓库内单头文件
# （third_party/nlohmann/json.hpp，MIT 许可，版本 3.10.5）
find_package(nlohmann_json QUIET)
if(nlohmann_json_FOUND)
  target_link_libraries(slotnexus_protocol PUBLIC nlohmann_json::nlohmann_json)
else()
  target_include_directories(slotnexus_protocol SYSTEM PUBLIC
    ${PROJECT_SOURCE_DIR}/third_party)
endif()

target_link_libraries(slotnexus_protocol PUBLIC slotnexus::common)
target_compile_features(slotnexus_protocol PUBLIC cxx_std_17)
