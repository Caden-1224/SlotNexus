// FakeLlmBackend：确定性流式文本生成（测试协议与编排，不模拟真实模型）。
//
// 确定性规则：
//   - generate(prompt) 按空白把 prompt 切分为 token（连续空白折叠），
//     逐 token 产出 kToken，全部 token 后产出 kDone；
//   - kDone 携带完整输出文本 = token 以单空格连接（即 prompt 的空白归一化）；
//   - cancel() 后 generate 不再产出任何事件；
//   - set_event_callback 开启新会话：重置取消状态。
// 流式输出（token 逐条产出）与最终文本都可精确断言。
#pragma once

#include <atomic>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "voxorchestra/backend/i_llm_backend.hpp"

namespace voxorchestra::backend::fake {

class FakeLlmBackend final : public ILlmBackend {
 public:
  void set_event_callback(EventCallback cb) override {
    cb_ = std::move(cb);
    cancelled_.store(false);
  }

  void generate(const std::string& prompt) override {
    if (!cb_ || cancelled_.load()) {
      return;
    }
    const auto tokens = split_words(prompt);
    std::string final_text;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      cb_({BackendEvent::Kind::kToken, tokens[i], {}});
      if (i > 0) {
        final_text += " ";
      }
      final_text += tokens[i];
    }
    cb_({BackendEvent::Kind::kDone, std::move(final_text), {}});
  }

  void cancel() override { cancelled_.store(true); }

 private:
  // 按空白切分（连续空白折叠），保持 token 顺序；空串返回空列表。
  static std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> words;
    std::istringstream iss(s);
    std::string w;
    while (iss >> w) {
      words.push_back(w);
    }
    return words;
  }

  EventCallback cb_;
  std::atomic<bool> cancelled_{false};
};

}  // namespace voxorchestra::backend::fake
