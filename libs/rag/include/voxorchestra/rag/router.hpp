// L0-L3 分级路由（RAG 决策层，纯函数、确定性）。
//
// 路由规则（由配置驱动，阈值可调）：
//   L0 紧急控制：查询命中 l0_keywords 任一关键词 → 绕过检索与 LLM，
//      直接返回固定控制应答；
//   L1 高置信事实：BM25 Top-1 得分 ≥ direct_threshold → 直接组织答案
//      （知识块文本），绕过 LLM（低延迟、低幻觉）；
//   L2 复杂问题：Top-1 得分 ≥ context_threshold（但 < direct_threshold）
//      → 注入 Top-K 知识块后调用 LLM（受控上下文）；
//   L3 闲聊/无命中：不注入任何知识，直接调用 LLM（不伪造知识命中）。
//
// 阈值必须用测试集标定（见单元测试与 config/mock/session.json），
// 不能盲调常数：每次 route 都返回命中块的得分，供记录与调参。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "voxorchestra/backend/i_retriever.hpp"
#include "voxorchestra/rag/bm25.hpp"
#include "voxorchestra/rag/knowledge_store.hpp"

namespace voxorchestra::rag {

enum class RouteLevel { kL0, kL1, kL2, kL3 };

inline const char* to_string(RouteLevel level) {
  switch (level) {
    case RouteLevel::kL0: return "l0";
    case RouteLevel::kL1: return "l1";
    case RouteLevel::kL2: return "l2";
    case RouteLevel::kL3: return "l3";
  }
  return "unknown";
}

struct RouterConfig {
  // 紧急控制触发词：在规范化查询文本中做子串匹配（不参与分词，
  // 避免 CJK 分词把双字词拆散），命中即 L0。
  std::vector<std::string> l0_keywords = {"停止", "取消", "暂停", "闭嘴",
                                          "stop", "cancel", "quit"};
  // Top-1 得分达到该值 → L1 直答（绕过 LLM）。
  // 默认值 4.5 由 data/knowledge/rag_test_set.jsonl 实测标定
  // （L1 期望集 top1 下界 5.22，见 artifacts/rag-baseline/calibration.md）。
  double direct_threshold = 4.5;
  // Top-1 得分达到该值（但未达 direct_threshold）→ L2 带上下文。
  // 默认值 2.0 标定同上（L2 期望集下界 2.98，L3 噪声上界 1.687）。
  double context_threshold = 2.0;
  // L2 注入 LLM 的上下文块数。
  std::size_t top_k = 2;
};

// 一次路由的完整决策：级别、命中块（含得分）、答案/提示词。
struct RouteDecision {
  RouteLevel level = RouteLevel::kL3;
  std::string query;                          // 规范化后的查询
  std::vector<backend::RetrievedChunk> chunks;  // 命中块（得分降序）
  double top1_score = 0.0;                    // Top-1 得分（L1/L2 用）
  std::string answer;                         // L0/L1 直接回答；L2/L3 为空
  std::string prompt;                         // L2/L3 送入 LLM；L0/L1 为空
};

class Router {
 public:
  // index：已 build 的 BM25 索引（entries 与 index 的文档一一对应，
  // 用于把命中下标映射回 id/text）。
  Router(Bm25Index index, std::vector<KnowledgeEntry> entries,
         RouterConfig config);

  // 对查询做 L0-L3 路由（纯函数：不修改内部状态，可并发调用）。
  RouteDecision route(const std::string& query) const;

  const Bm25Index& index() const { return index_; }
  const RouterConfig& config() const { return config_; }

 private:
  // 命中列表 → 统一块结构（id/text 来自知识条目，得分降序）。
  std::vector<backend::RetrievedChunk> make_chunks(
      const std::vector<Bm25Index::Hit>& hits) const;

  Bm25Index index_;
  std::vector<KnowledgeEntry> entries_;
  RouterConfig config_;
};

}  // namespace voxorchestra::rag
