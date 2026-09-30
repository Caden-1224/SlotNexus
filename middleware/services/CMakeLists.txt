# 通用服务层：node host、unit manager、edge gateway 与 echo 示例。
#
# Author: Caden
 # include 保存多个服务共享的公共头；其余目录按服务职责组织。
install(FILES include/slotnexus/services/action_helpers.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/slotnexus/services)

add_subdirectory(node_host)
add_subdirectory(unit_manager)
add_subdirectory(edge_gateway)
add_subdirectory(echo_node)
