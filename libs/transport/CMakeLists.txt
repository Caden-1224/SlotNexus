# 传输库：ZeroMQ 的 REQ/REP、PUB/SUB、PUSH/PULL 封装
#
# 依赖 libzmq（系统包 libzmq3-dev，MPL-2.0 动态链接例外）。CMake 3.22 的
# FindZeroMQ 模块需要 pkg-config，本机未安装，因此手动定位头文件与库。
add_library(slotnexus_transport STATIC
  src/rpc.cpp
  src/pubsub.cpp
  src/pushpull.cpp
)
add_library(slotnexus::transport ALIAS slotnexus_transport)

find_path(ZMQ_INCLUDE_DIR NAMES zmq.h PATHS /usr/include)
find_path(ZMQ_CPP_INCLUDE_DIR NAMES zmq.hpp PATHS /usr/include)
find_library(ZMQ_LIBRARY NAMES zmq PATHS /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu)

if(NOT ZMQ_INCLUDE_DIR OR NOT ZMQ_CPP_INCLUDE_DIR OR NOT ZMQ_LIBRARY)
  message(FATAL_ERROR "找不到 libzmq（zmq.h/zmq.hpp/libzmq）。请安装系统包 libzmq3-dev。")
endif()

target_include_directories(slotnexus_transport PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_include_directories(slotnexus_transport SYSTEM PUBLIC
  ${ZMQ_INCLUDE_DIR}
)
target_link_libraries(slotnexus_transport PUBLIC slotnexus::common ${ZMQ_LIBRARY})
target_compile_features(slotnexus_transport PUBLIC cxx_std_17)
