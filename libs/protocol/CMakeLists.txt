# 协议库：统一消息 MessageEnvelope 的 JSON 编码/解码与校验
add_library(voxorchestra_protocol STATIC
  src/message_envelope.cpp
)
add_library(voxorchestra::protocol ALIAS voxorchestra_protocol)

target_include_directories(voxorchestra_protocol PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# nlohmann/json：优先使用包管理器提供的 cmake 配置，找不到时退回裸头文件
find_package(nlohmann_json QUIET)
if(nlohmann_json_FOUND)
  target_link_libraries(voxorchestra_protocol PUBLIC nlohmann_json::nlohmann_json)
else()
  target_include_directories(voxorchestra_protocol SYSTEM PUBLIC /usr/include)
endif()

target_link_libraries(voxorchestra_protocol PUBLIC voxorchestra::common)
target_compile_features(voxorchestra_protocol PUBLIC cxx_std_17)
