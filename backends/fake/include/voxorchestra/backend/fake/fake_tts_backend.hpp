// FakeTtsBackend：确定性流式语音合成（测试协议与编排，不模拟真实模型）。
//
// 确定性规则：
//   - synthesize(text) 产出块数 = max(1, ⌈文本字节数 / 32⌉) 个 kPcm 帧，
//     每帧 320 采样（20 ms @ 16 kHz）；
//   - 波形为 500 Hz 方波（每 32 采样一周期、高低电平各 16 采样），幅度
//     ±6000（约 -5.5 dBFS，人耳可清晰听见）；相位按全局采样序号连续，
//     跨块不跳变。采样值 = (全局序号 % 32 < 16) ? 6000 : -6000，
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
        const std::size_t g = c * static_cast<std::size_t>(kFrameSamples) + s;
        pcm[s] = (g % 32 < 16) ? 6000 : -6000;  // 500 Hz 方波，相位跨块连续
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
