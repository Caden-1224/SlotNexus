# Session 编排库：状态机与编排管线（不做模型推理）。
# 依赖 RAG 路由与 Backend 契约接口；队列/分句/WAV 来自 common。
add_library(voxorchestra_session STATIC
  src/session_pipeline.cpp
  src/session_state_machine.cpp
)
add_library(voxorchestra::session ALIAS voxorchestra_session)

target_include_directories(voxorchestra_session PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(voxorchestra_session PUBLIC
  voxorchestra::common
  voxorchestra::rag
  voxorchestra::backend
)
target_compile_features(voxorchestra_session PUBLIC cxx_std_17)
