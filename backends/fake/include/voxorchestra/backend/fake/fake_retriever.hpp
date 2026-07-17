// FakeRetriever：确定性 Top-K 文本检索（测试检索链路的顺序与截断语义）。
//
// 语义：
//   - 知识库由构造注入，缺省时使用内置示例条目；
//   - 得分与查询无关（每条目固定得分），排序规则：得分降序，同分按 id
//     升序（全序，保证确定性）；
//   - retrieve 为无副作用只读操作：相同输入永远产出相同输出。
// 不是真实检索模型（BM25 于编排阶段接入），只验证检索接口与数据形状。
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "voxorchestra/backend/i_retriever.hpp"

namespace voxorchestra::backend::fake {

// 内置示例知识库（少量条目，供协议与编排测试）。
inline std::vector<RetrievedChunk> DefaultKnowledge() {
  return {
      {"k-voxorchestra", "VoxOrchestra 是端侧全离线语音交互中间件", 0.95},
      {"k-arch", "VoxOrchestra 使用多进程架构：网关、管理器与节点各自独立", 0.90},
      {"k-backend", "VoxOrchestra 的 Backend 是可替换实现，Fake 用于测试协议", 0.85},
      {"k-rag", "VoxOrchestra 的 RAG 路由分为 L0 到 L3 四层", 0.80},
      {"k-audio", "VoxOrchestra 音频格式统一为 16kHz 单声道 16-bit PCM", 0.75},
  };
}

class FakeRetriever final : public IRetriever {
 public:
  // 默认使用内置示例知识库。
  FakeRetriever() : knowledge_(DefaultKnowledge()) {}

  // 使用外部注入的知识库（复制一份，不持有外部引用）。
  explicit FakeRetriever(std::vector<RetrievedChunk> knowledge)
      : knowledge_(std::move(knowledge)) {}

  std::vector<RetrievedChunk> retrieve(const std::string& /*query*/,
                                       std::size_t top_k) override {
    if (top_k == 0) {
      return {};
    }
    std::vector<RetrievedChunk> result = knowledge_;
    std::sort(result.begin(), result.end(), [](const RetrievedChunk& a,
                                               const RetrievedChunk& b) {
      if (a.score != b.score) {
        return a.score > b.score;  // 得分降序
      }
      return a.id < b.id;  // 同分按 id 升序（全序）
    });
    if (result.size() > top_k) {
      result.resize(top_k);
    }
    return result;
  }

 private:
  std::vector<RetrievedChunk> knowledge_;
};

}  // namespace voxorchestra::backend::fake
