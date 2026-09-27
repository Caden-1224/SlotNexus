# 语音故障注入测试。
#
# Author: Caden
function(vox_voice_fault_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE
    slotnexus::protocol
    slotnexus::voice_common
    slotnexus::voice_backend
    slotnexus::voice_session
    ${ARGN})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "${VOX_TEST_ENV}")
  set_target_properties(${name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/fault)
endfunction()

vox_voice_fault_test(session_fault_test voice_session_node_lib slotnexus::voice_fake_backends)
target_include_directories(session_fault_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../nodes/session_node)
vox_voice_fault_test(chain_fault_test)
