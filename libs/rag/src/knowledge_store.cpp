#include "voxorchestra/rag/knowledge_store.hpp"

#include <fstream>
#include <sstream>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

namespace voxorchestra::rag {

namespace {

// 读取文件全部内容；失败返回 false（文件不存在或读取错误）。
bool read_file(const std::string& path, std::string& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

}  // namespace

KnowledgeStore::KnowledgeStore(const std::string& jsonl_path) {
  std::string content;
  if (!read_file(jsonl_path, content)) {
    throw KnowledgeStoreError("无法读取知识库文件: " + jsonl_path);
  }

  std::unordered_set<std::string> seen_ids;
  std::istringstream lines(content);
  std::string line;
  std::size_t line_no = 0;
  std::size_t entry_no = 0;  // 逻辑条目序号（跳过注释/空行后递增，id 稳定）
  while (std::getline(lines, line)) {
    ++line_no;
    if (line.empty()) {
      continue;
    }
    if (line[0] == '#') {
      continue;  // 注释行
    }
    ++entry_no;
    nlohmann::json obj;
    try {
      obj = nlohmann::json::parse(line);
    } catch (const nlohmann::json::exception&) {
      throw KnowledgeStoreError(jsonl_path + ":" + std::to_string(line_no) +
                                " JSON 解析失败");
    }
    if (!obj.is_object()) {
      throw KnowledgeStoreError(jsonl_path + ":" + std::to_string(line_no) +
                                " 必须为 JSON 对象");
    }
    const auto text_it = obj.find("text");
    if (text_it == obj.end() || !text_it->is_string()) {
      throw KnowledgeStoreError(jsonl_path + ":" + std::to_string(line_no) +
                                " 缺少字符串字段 text");
    }
    std::string id;
    const auto id_it = obj.find("id");
    if (id_it != obj.end() && id_it->is_string()) {
      id = id_it->get<std::string>();
    } else {
      id = "k" + std::to_string(entry_no);
    }
    if (!seen_ids.insert(id).second) {
      throw KnowledgeStoreError(jsonl_path + ":" + std::to_string(line_no) +
                                " id 重复: " + id);
    }
    entries_.push_back({std::move(id), text_it->get<std::string>()});
  }
}

}  // namespace voxorchestra::rag
