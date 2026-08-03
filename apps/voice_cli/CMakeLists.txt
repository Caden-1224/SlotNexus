# 外部客户端：TCP NDJSON → Gateway（联调入口，文本/WAV 两种输入 + 打断验证）。
add_executable(voice_cli main.cpp)
target_link_libraries(voice_cli PRIVATE voxorchestra::protocol)
