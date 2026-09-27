# 语音集成测试。
#
# Author: Caden
add_executable(node_events_test node_events_test.cpp)
target_link_libraries(node_events_test PRIVATE
  slotnexus::dataplane slotnexus::transport slotnexus::voice_common)
add_test(NAME node_events_test COMMAND node_events_test)
set_tests_properties(node_events_test PROPERTIES TIMEOUT 15 ENVIRONMENT "${VOX_TEST_ENV}")
set_target_properties(node_events_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/integration)

add_executable(fake_nodes_e2e_test fake_nodes_e2e_test.cpp)
target_link_libraries(fake_nodes_e2e_test PRIVATE slotnexus::protocol)
add_test(NAME fake_nodes_e2e_test COMMAND fake_nodes_e2e_test)
set_tests_properties(fake_nodes_e2e_test PROPERTIES ENVIRONMENT "${VOX_TEST_ENV}")
set_target_properties(fake_nodes_e2e_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/integration)

foreach(build_mode IN ITEMS default hardware missing)
  add_test(
    NAME taishanpi3m_build_${build_mode}_test
    COMMAND bash
            ${CMAKE_CURRENT_SOURCE_DIR}/taishanpi3m_build_script_test.sh
            ${build_mode}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../deploy/taishanpi3m/build.sh
  )
  set_tests_properties(taishanpi3m_build_${build_mode}_test PROPERTIES TIMEOUT 10 ENVIRONMENT "${VOX_TEST_ENV}")
endforeach()

foreach(check_case IN ITEMS valid missing_model unresolved_library)
  add_test(
    NAME taishanpi3m_deploy_${check_case}_test
    COMMAND bash
            ${CMAKE_CURRENT_SOURCE_DIR}/taishanpi3m_deploy_check_test.sh
            ${check_case}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../deploy/taishanpi3m/check_deployment.sh
  )
  set_tests_properties(taishanpi3m_deploy_${check_case}_test PROPERTIES TIMEOUT 10 ENVIRONMENT "${VOX_TEST_ENV}")
endforeach()

foreach(stop_case IN ITEMS stop_idempotent stop_force)
  add_test(
    NAME taishanpi3m_${stop_case}_test
    COMMAND bash
            ${CMAKE_CURRENT_SOURCE_DIR}/taishanpi3m_lifecycle_test.sh
            ${stop_case}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../deploy/taishanpi3m/start.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/../../deploy/taishanpi3m/stop.sh
  )
  set_tests_properties(taishanpi3m_${stop_case}_test PROPERTIES
    TIMEOUT 15 RUN_SERIAL TRUE ENVIRONMENT "${VOX_TEST_ENV}")
endforeach()

foreach(start_case IN ITEMS start_success missing_runtime setup_failure)
  add_test(
    NAME taishanpi3m_${start_case}_test
    COMMAND bash
            ${CMAKE_CURRENT_SOURCE_DIR}/taishanpi3m_lifecycle_test.sh
            ${start_case}
            ${CMAKE_CURRENT_SOURCE_DIR}/../../deploy/taishanpi3m/start.sh
            ${CMAKE_CURRENT_SOURCE_DIR}/../../deploy/taishanpi3m/stop.sh
  )
  set_tests_properties(taishanpi3m_${start_case}_test PROPERTIES
    TIMEOUT 15 RUN_SERIAL TRUE ENVIRONMENT "${VOX_TEST_ENV}")
endforeach()
