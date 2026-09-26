// 文本检索后端契约（同步，非流式）。
//
// 返回按相关度降序的 Top-K 块，供 L2 路由注入受控上下文（BM25）。
// 检索为无副作用只读操作，可被多次调用；实现必须确定性（相同输入 → 相同输出）。
//
// 可替换实现：FakeRetriever（默认） / 未来 BM25Retriever。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace slotnexus::backend {

// 检索命中的知识块。
struct RetrievedChunk {
 std::string id;      // 知识条目 id
 std::string text;    // 块文本（注入 LLM 上下文）
 double score = 0.0;  // 相关度得分（数值大者更相关）
};

class IRetriever {
public:
 virtual ~IRetriever() = default;

 // 返回 top_k 个最相关块（降序）；知识库为空返回空列表，top_k 为 0 返回空列表。
 virtual std::vector<RetrievedChunk> retrieve(const std::string& query,
                                              std::size_t top_k) = 0;
};

}  // namespace slotnexus::backend
