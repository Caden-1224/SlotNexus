# 核心单元测试。
#
# Author: Caden
function(vox_core_unit_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE ${ARGN})
  add_test(NAME ${name} COMMAND ${name})
  set_target_properties(${name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests/unit)
endfunction()

vox_core_unit_test(smoke_test slotnexus::common)
vox_core_unit_test(protocol_test slotnexus::protocol)
vox_core_unit_test(frame_decoder_test slotnexus::network)
vox_core_unit_test(reactor_test slotnexus::network)
vox_core_unit_test(task_registry_test slotnexus::task_registry)
vox_core_unit_test(runtime_test slotnexus::runtime)
vox_core_unit_test(bounded_queue_test slotnexus::common)
