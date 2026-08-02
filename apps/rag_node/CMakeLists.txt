# 真实 BM25 检索节点：RuntimeNode 外壳 + Router（知识库/L0-L3 路由）适配器。
add_executable(rag_node main.cpp)
target_link_libraries(rag_node PRIVATE voxorchestra::app_common voxorchestra::rag)
