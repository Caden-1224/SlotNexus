# 语音 RAG：JSONL 知识库、文本规范化、BM25 与 L0-L3 路由。
#
# Author: Caden
add_library(slotnexus_voice_rag STATIC
  src/bm25.cpp
  src/knowledge_store.cpp
  src/router.cpp
  src/text_normalizer.cpp
)
add_library(slotnexus::voice_rag ALIAS slotnexus_voice_rag)
add_library(slotnexus::rag ALIAS slotnexus_voice_rag)
set_target_properties(slotnexus_voice_rag PROPERTIES EXPORT_NAME voice_rag)
target_include_directories(slotnexus_voice_rag PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(slotnexus_voice_rag PUBLIC
  slotnexus::json)
target_compile_features(slotnexus_voice_rag PUBLIC cxx_std_17)
