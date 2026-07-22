#include "voxorchestra/rag/router.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "voxorchestra/rag/text_normalizer.hpp"

namespace voxorchestra::rag {

Router::Router(Bm25Index index, std::vector<KnowledgeEntry> entries,
               RouterConfig config)
    : index_(std::move(index)),
      entries_(std::move(entries)),
      config_(std::move(config)) {}

RouteDecision Router::route(const std::string& query) const {
  const std::string normalized = normalize_text(query);
  RouteDecision decision;
  decision.query = normalized;

  // L0：紧急控制关键词子串匹配（规范化文本上匹配，关键词自身也规范化）。
  for (const auto& kw : config_.l0_keywords) {
    if (normalized.find(normalize_text(kw)) != std::string::npos) {
      decision.level = RouteLevel::kL0;
      decision.answer = "已停止当前播放，随时待命";
      return decision;
    }
  }

  // BM25 检索（L1/L2 共用同一排名，保证阈值语义一致）。
  const auto hits = index_.top_k(normalized, config_.top_k);
  decision.chunks = make_chunks(hits);
  if (!hits.empty()) {
    decision.top1_score = hits.front().score;
  }

  if (decision.top1_score >= config_.direct_threshold) {
    // L1：高置信直答，绕过 LLM。
    decision.level = RouteLevel::kL1;
    decision.answer = decision.chunks.front().text;
    return decision;
  }
  if (decision.top1_score >= config_.context_threshold) {
    // L2：带受控上下文调用 LLM。
    decision.level = RouteLevel::kL2;
    std::string context;
    for (const auto& c : decision.chunks) {
      context += "- " + c.text + "\n";
    }
    decision.prompt = normalized + "\n\n请依据上面的参考知识回答，不要编造参考知识之外的内容。";
    decision.prompt = context + decision.prompt;
    return decision;
  }

  // L3：闲聊或无命中，不注入知识。
  decision.level = RouteLevel::kL3;
  decision.prompt = normalized;
  return decision;
}

std::vector<backend::RetrievedChunk> Router::make_chunks(
    const std::vector<Bm25Index::Hit>& hits) const {
  std::vector<backend::RetrievedChunk> chunks;
  for (const auto& hit : hits) {
    // index 与 entries 按构造时的顺序一一对应。
    if (hit.doc_index < entries_.size()) {
      chunks.push_back({entries_[hit.doc_index].id,
                        entries_[hit.doc_index].text, hit.score});
    }
  }
  return chunks;
}

}  // namespace voxorchestra::rag
