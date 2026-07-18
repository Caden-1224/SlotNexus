// FakeTtsBackend：确定性流式语音合成（测试协议与编排，不模拟真实模型）。
//
// 确定性规则：
//   - synthesize(text) 产出块数 = max(1, ⌈文本字节数 / 32⌉) 个 kPcm 帧，
//     每帧 320 采样（20 ms @ 16 kHz）；
//   - 采样值 = ((块序号*613 + 采样下标*311) mod 2048) - 1024，
//     完全由（块序号, 采样下标）决定，可逐采样断言；
//   - 全部帧后产出 kDone（不携带数据）；
//   - cancel() 后 synthesize 不再产出任何事件；
//   - set_event_callback 开启新会话：重置取消状态。
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "voxorchestra/backend/i_tts_backend.hpp"

namespace voxorchestra::backend::fake {

class FakeTtsBackend final : public ITtsBackend {
 public:
  void set_event_callback(EventCallback cb) override {
    cb_ = std::move(cb);
    cancelled_.store(false);
  }

  void synthesize(const std::string& text) override {
    if (!cb_ || cancelled_.load()) {
      return;
    }
    const std::size_t chunk_count =
        std::max<std::size_t>(1, (text.size() + 31) / 32);  // ⌈字节数/32⌉
    for (std::size_t c = 0; c < chunk_count; ++c) {
      std::vector<int16_t> pcm(static_cast<std::size_t>(kFrameSamples));
      for (std::size_t s = 0; s < pcm.size(); ++s) {
        pcm[s] = static_cast<int16_t>(
            ((c * 613 + s * 311) % 2048) - 1024);
      }
      cb_({BackendEvent::Kind::kPcm, {}, std::move(pcm)});
    }
    cb_({BackendEvent::Kind::kDone, {}, {}});
  }

  void cancel() override { cancelled_.store(true); }

 private:
  EventCallback cb_;
  std::atomic<bool> cancelled_{false};
};

}  // namespace voxorchestra::backend::fake
