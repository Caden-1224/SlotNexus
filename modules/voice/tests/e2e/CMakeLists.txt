# 语音端到端测试。
#
# Author: Caden
function(vox_voice_e2e_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE
    slotnexus::protocol
    slotnexus::voice_common
    slotnexus::voice_backend
    ${ARGN})
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "${VOX_TEST_ENV}")
  set_target_properties(${name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/e2e)
endfunction()

vox_voice_e2e_test(session_e2e_test)
vox_voice_e2e_test(session_rounds_test)
vox_voice_e2e_test(session_net_e2e_test)
