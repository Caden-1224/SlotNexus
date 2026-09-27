# 协议库：MessageEnvelope JSON 编解码与校验。
#
# Author: Caden
add_library(slotnexus_protocol STATIC
  src/message_envelope.cpp
)
add_library(slotnexus::protocol ALIAS slotnexus_protocol)
set_target_properties(slotnexus_protocol PROPERTIES EXPORT_NAME protocol)
target_include_directories(slotnexus_protocol PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(slotnexus_protocol PUBLIC slotnexus::common slotnexus::json)
target_compile_features(slotnexus_protocol PUBLIC cxx_std_17)
install(TARGETS slotnexus_protocol EXPORT slotnexus-core-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
