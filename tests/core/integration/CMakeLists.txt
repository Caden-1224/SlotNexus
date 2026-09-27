# 核心集成/E2E 测试。
#
# Author: Caden
function(vox_core_integration_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE ${ARGN})
  add_test(NAME ${name} COMMAND ${name})
  set_target_properties(${name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/integration)
endfunction()

vox_core_integration_test(rpc_integration_test slotnexus::transport)
vox_core_integration_test(pubsub_integration_test slotnexus::transport)
vox_core_integration_test(pushpull_integration_test slotnexus::transport)
vox_core_integration_test(dataplane_test slotnexus::dataplane)
vox_core_integration_test(tcp_server_test slotnexus::network)
vox_core_integration_test(gateway_test slotnexus::edge_gateway slotnexus::node_host)
vox_core_integration_test(echo_e2e_test slotnexus::protocol)
vox_core_integration_test(unit_manager_module_test
  slotnexus::unit_manager slotnexus::transport)

add_test(
  NAME no_hw_deps_guard_test
  COMMAND bash
          ${CMAKE_CURRENT_SOURCE_DIR}/check_no_hw_deps_test.sh
          ${CMAKE_SOURCE_DIR}/scripts/check_no_hw_deps.sh
          ${CMAKE_CXX_COMPILER}
)
set_tests_properties(no_hw_deps_guard_test PROPERTIES TIMEOUT 10)

add_test(
  NAME core_boundary_test
  COMMAND bash
          ${CMAKE_SOURCE_DIR}/scripts/check_core_boundary.sh
          ${CMAKE_SOURCE_DIR}
)
set_tests_properties(core_boundary_test PROPERTIES TIMEOUT 10)
