# Fake 文本检索节点：RuntimeNode 外壳 + FakeRetriever 适配器。
add_executable(rag_node main.cpp)
target_link_libraries(rag_node PRIVATE voxorchestra::app_common voxorchestra::fake_backends)
