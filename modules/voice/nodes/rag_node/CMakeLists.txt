# RAG 检索节点：通用 RuntimeNode 外壳 + 语音 RAG 适配器。
#
# Author: Caden
add_executable(rag_node main.cpp)
target_link_libraries(rag_node PRIVATE
  slotnexus::node_host
  slotnexus::voice_rag)
set_target_properties(rag_node PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/rag_node)
