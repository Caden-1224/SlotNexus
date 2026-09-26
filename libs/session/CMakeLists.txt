# Session 编排库：状态机与编排管线（不做模型推理）。
# 依赖 RAG 路由与 Backend 契约接口；队列/分句/WAV 来自 common。
add_library(slotnexus_session STATIC
  src/session_pipeline.cpp
  src/session_state_machine.cpp
)
add_library(slotnexus::session ALIAS slotnexus_session)

target_include_directories(slotnexus_session PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(slotnexus_session PUBLIC
  slotnexus::common
  slotnexus::rag
  slotnexus::backend
)
target_compile_features(slotnexus_session PUBLIC cxx_std_17)
