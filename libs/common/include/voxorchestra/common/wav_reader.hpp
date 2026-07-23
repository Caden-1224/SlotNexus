// WAV 文件读取器（16-bit PCM，RIFF/WAVE 格式解析）。
//
// 解析范围：RIFF/WAVE 头 + fmt 块 + data 块；未知块（LIST/fact/bext 等）
// 按块结构跳过；压缩格式必须为 PCM（值为 1），否则报错。读取器返回头部
// 信息与全部采样，由调用方校验是否满足 16kHz 单声道约定
// （backend::kSampleRateHz/kChannels）；读取器本身保持通用。
//
// 错误一律返回 false + 人类可读原因，不抛异常；输入损坏不会崩溃。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace voxorchestra::common {

struct WavInfo {
  std::uint32_t sample_rate = 0;
  std::uint16_t channels = 0;
  std::uint16_t bits = 0;
  std::vector<std::int16_t> samples;  // 全部 PCM 采样（16-bit 有符号）
};

struct WavReadResult {
  bool ok = false;
  std::string error;  // ok 为 false 时的原因
  WavInfo info;
};

class WavReader {
 public:
  // 读取并解析 WAV 文件；返回 ok=true 且 info 填充完整。
  static WavReadResult read(const std::string& path);
};

}  // namespace voxorchestra::common
