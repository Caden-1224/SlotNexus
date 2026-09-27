# 语音模块测试入口。
#
# Author: Caden
# 仅当 VOX_BUILD_VOICE=ON 时由顶层构建；所有测试通过公开 voice target 消费
# 核心，不再直接 include 核心源码目录。
get_filename_component(VOX_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
include_directories(${VOX_REPO_ROOT}/tests/support)

# 最小默认测试资产根：knowledge.jsonl 为语音模块运行示例副本，voice.wav
# 由脚本生成，session.json 从模块配置复制。真实 demo_zh.wav / 专用
# rag_test_set.jsonl 不参与默认测试。
set(VOX_TEST_ROOT "${CMAKE_BINARY_DIR}/test-root")
file(MAKE_DIRECTORY
  "${VOX_TEST_ROOT}/data/knowledge"
  "${VOX_TEST_ROOT}/data/fixtures"
  "${VOX_TEST_ROOT}/config/mock")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../examples/knowledge.jsonl
  "${VOX_TEST_ROOT}/data/knowledge/knowledge.jsonl" COPYONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../config/mock/session.json
  "${VOX_TEST_ROOT}/config/mock/session.json" COPYONLY)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_command(
  OUTPUT "${VOX_TEST_ROOT}/data/fixtures/voice.wav"
  COMMAND ${Python3_EXECUTABLE} ${VOX_REPO_ROOT}/scripts/gen_fixture_wav.py
          "${VOX_TEST_ROOT}/data/fixtures/voice.wav"
  DEPENDS ${VOX_REPO_ROOT}/scripts/gen_fixture_wav.py
  COMMENT "生成确定性 voice.wav 测试夹具")
add_custom_target(vox_voice_test_assets ALL
  DEPENDS "${VOX_TEST_ROOT}/data/fixtures/voice.wav")
set(VOX_TEST_ENV "VOXORCHESTRA_TEST_ROOT=${VOX_TEST_ROOT}")

add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(e2e)
add_subdirectory(fault)
