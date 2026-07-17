// FakeAudioSource：确定性麦克风模拟（合成 16-bit 单声道 PCM 帧）。
//
// 帧内容完全由（帧序号, 采样下标）决定，不含随机源：相同参数永远产出
// 相同数据，供协议、状态机与编排测试断言精确内容。不是真实采集设备。
#pragma once

#include <cstdint>
#include <vector>

#include "voxorchestra/backend/backend_event.hpp"

namespace voxorchestra::backend::fake {

class FakeAudioSource {
 public:
  // 生成第 seq 帧（默认 20 ms 帧 = 320 采样）。
  // 采样值 = ((seq*977 + 下标*197) mod 2048) - 1024，范围 [-1024, 1023]。
  static std::vector<int16_t> make_frame(std::uint32_t seq,
                                         int samples = kFrameSamples) {
    std::vector<int16_t> frame(static_cast<std::size_t>(samples));
    for (int s = 0; s < samples; ++s) {
      frame[static_cast<std::size_t>(s)] = static_cast<int16_t>(
          ((seq * 977 + static_cast<std::uint32_t>(s) * 197) % 2048) - 1024);
    }
    return frame;
  }
};

}  // namespace voxorchestra::backend::fake
