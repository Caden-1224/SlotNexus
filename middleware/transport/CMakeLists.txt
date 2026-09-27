# 传输库：ZeroMQ REQ/REP、PUB/SUB、PUSH/PULL 封装。
#
# Author: Caden
add_library(slotnexus_transport STATIC
  src/rpc.cpp
  src/pubsub.cpp
  src/pushpull.cpp
)
add_library(slotnexus::transport ALIAS slotnexus_transport)
set_target_properties(slotnexus_transport PROPERTIES EXPORT_NAME transport)
find_path(ZMQ_INCLUDE_DIR NAMES zmq.h)
find_path(ZMQ_CPP_INCLUDE_DIR NAMES zmq.hpp)
find_library(ZMQ_LIBRARY NAMES zmq)
if(NOT ZMQ_INCLUDE_DIR OR NOT ZMQ_CPP_INCLUDE_DIR OR NOT ZMQ_LIBRARY)
  message(FATAL_ERROR "找不到 libzmq（zmq.h/zmq.hpp/libzmq）。请安装系统包 libzmq3-dev。")
endif()
target_include_directories(slotnexus_transport PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(slotnexus_transport SYSTEM PUBLIC
  $<BUILD_INTERFACE:${ZMQ_INCLUDE_DIR}>)
target_link_libraries(slotnexus_transport PUBLIC
  slotnexus::common
  ${ZMQ_LIBRARY})
target_compile_features(slotnexus_transport PUBLIC cxx_std_17)
install(TARGETS slotnexus_transport EXPORT slotnexus-core-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
