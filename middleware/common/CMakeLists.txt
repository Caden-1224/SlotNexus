# 通用基础库：版本、有界队列、日志、base64 等无业务类型工具。
#
# Author: Caden
add_library(slotnexus_common STATIC
  src/version.cpp
)
add_library(slotnexus::common ALIAS slotnexus_common)
set_target_properties(slotnexus_common PROPERTIES EXPORT_NAME common)
target_include_directories(slotnexus_common PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(slotnexus_common PUBLIC cxx_std_17)
install(TARGETS slotnexus_common EXPORT slotnexus-core-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
