#!/bin/bash
# RAG 固定测试集复现入口（证据）：库级 21 条断言 + 节点级集成验证。
#
# 运行内容：
#   1. 构建 rag_testset_test / fake_nodes_e2e_test；
#   2. rag_testset_test --summary artifacts/rag-baseline/summary.json
#      （逐条路由断言 + 校准表 JSON）；
#   3. fake_nodes_e2e_test（真实 rag_node 进程经 Manager/网关全链路 L1 直答）。
# 阈值（4.5/2.0）与知识库、测试集三者配套冻结，改动任一需重新标定：
#   见 artifacts/rag-baseline/calibration.md。
#
# 用法：scripts/run_rag_testset.sh
set -u
cd "$(dirname "$0")/.."
B=build-wsl

echo "== 构建 =="
cmake --build "$B" --target rag_testset_test fake_nodes_e2e_test -j"$(nproc)" > /dev/null

echo "== 库级：21 条固定测试集（知识库 knowledge.jsonl，阈值 4.5/2.0）=="
"$B/tests/unit/rag_testset_test" --summary artifacts/rag-baseline/summary.json
RC_RAG=$?

echo
echo "== 节点级：真实 rag_node 进程经五节点链路（Manager 轮转 + 网关）=="
"$B/tests/integration/fake_nodes_e2e_test"
RC_NODE=$?

if [ "$RC_RAG" -ne 0 ] || [ "$RC_NODE" -ne 0 ]; then
 echo "RAG 测试集复现失败（rag=$RC_RAG node=$RC_NODE）"
 exit 1
fi
echo "RAG 测试集复现完成：21/21 通过，校准表见 artifacts/rag-baseline/summary.json"
