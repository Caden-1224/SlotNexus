// FakeAsrBackend：确定性流式语音识别（测试协议与编排，不模拟真实模型）。
//
// 确定性规则：
//   - 每帧 feed_audio 产出 kPartial，文本 = "第<N>帧(<采样数>)"；
//   - is_last 帧处理后产出 kFinal，文本 = 全部 partial 以空格连接；
//   - cancel() 后不再产出任何事件，feed_audio 为空操作；
//   - set_event_callback 开启新会话：重置帧计数、累计文本与取消状态。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "voxorchestra/backend/i_asr_backend.hpp"

namespace voxorchestra::backend::fake {

class FakeAsrBackend final : public IAsrBackend {
 public:
  void set_event_callback(EventCallback cb) override {
    cb_ = std::move(cb);
    frame_count_ = 0;
    partials_.clear();
    cancelled_.store(false);
  }

  void feed_audio(const std::vector<int16_t>& pcm, bool is_last) override {
    if (!cb_ || cancelled_.load()) {
      return;
    }
    ++frame_count_;
    const std::string partial =
        "第" + std::to_string(frame_count_) + "帧(" + std::to_string(pcm.size()) + ")";
    partials_.push_back(partial);
    cb_({BackendEvent::Kind::kPartial, partial, {}});

    if (is_last) {
      std::string final_text;
      for (std::size_t i = 0; i < partials_.size(); ++i) {
        if (i > 0) {
          final_text += " ";
        }
        final_text += partials_[i];
      }
      cb_({BackendEvent::Kind::kFinal, std::move(final_text), {}});
    }
  }

  void cancel() override { cancelled_.store(true); }

 private:
  EventCallback cb_;
  std::atomic<bool> cancelled_{false};
  int frame_count_ = 0;
  std::vector<std::string> partials_;
};

}  // namespace voxorchestra::backend::fake
