// JSONL 知识库（RAG 数据源，格式简单可审查）。
//
// 每行一个 JSON 对象，字段：
//   {"id": "k-xxx", "text": "知识条目内容"}     # id 可选，缺省用 "k<行号>"
// 空行与以 # 开头的行忽略。text 缺失、JSON 非法或 id 重复均抛
// KnowledgeStoreError（带行号），保证知识库内容可审计、失败可见。
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace voxorchestra::rag {

// 知识条目：id 唯一，text 为可注入 LLM 的块文本。
struct KnowledgeEntry {
  std::string id;
  std::string text;
};

// 知识库加载错误（含行号，便于定位 JSONL 中的问题行）。
class KnowledgeStoreError : public std::runtime_error {
 public:
  explicit KnowledgeStoreError(const std::string& message)
      : std::runtime_error(message) {}
};

class KnowledgeStore {
 public:
  // 从 JSONL 文件加载；文件不存在或解析失败抛 KnowledgeStoreError。
  explicit KnowledgeStore(const std::string& jsonl_path);

  const std::vector<KnowledgeEntry>& entries() const { return entries_; }
  std::size_t size() const { return entries_.size(); }

 private:
  std::vector<KnowledgeEntry> entries_;
};

}  // namespace voxorchestra::rag
