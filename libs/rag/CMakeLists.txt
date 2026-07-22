# 轻量 RAG 库：JSONL 知识库、文本规范化、BM25 打分与 L0-L3 分级路由。
# 依赖统一块结构 RetrievedChunk（backends 契约头，无厂商 SDK）。
add_library(voxorchestra_rag STATIC
  src/bm25.cpp
  src/knowledge_store.cpp
  src/router.cpp
  src/text_normalizer.cpp
)
add_library(voxorchestra::rag ALIAS voxorchestra_rag)

target_include_directories(voxorchestra_rag PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(voxorchestra_rag PUBLIC voxorchestra::backend)

# nlohmann/json：优先使用包管理器提供的 cmake 配置，找不到时退回仓库内单头文件。
find_package(nlohmann_json QUIET)
if(nlohmann_json_FOUND)
  target_link_libraries(voxorchestra_rag PUBLIC nlohmann_json::nlohmann_json)
else()
  target_include_directories(voxorchestra_rag SYSTEM PUBLIC
    ${PROJECT_SOURCE_DIR}/third_party)
endif()

target_compile_features(voxorchestra_rag PUBLIC cxx_std_17)
