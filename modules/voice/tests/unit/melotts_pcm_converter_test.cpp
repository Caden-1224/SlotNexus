// MeloTtsPcmConverter 输出语义回归（纯 CPU，硬件后端构建下运行）。
// Author: Caden
//
// 保护的不变量：
//   1. 分块 push 与一次性 push 得到逐样本一致的 16 kHz S16 输出；
//   2. 摊还搬移已消费前缀后，输出与逐输出搬移的独立朴素参考一致。
// 只有编码语义，不依赖模型或声卡。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "melotts_internal.hpp"

namespace {

namespace melo = slotnexus::backend::melotts;

std::int16_t to_s16(float sample) {
  if (!std::isfinite(sample)) {
    return 0;
  }
  const float scaled = std::max(-32768.0F, std::min(32767.0F, sample * 32768.0F));
  if (scaled >= 32767.0F) {
    return std::numeric_limits<std::int16_t>::max();
  }
  if (scaled <= -32768.0F) {
    return std::numeric_limits<std::int16_t>::min();
  }
  return static_cast<std::int16_t>(std::lrint(scaled));
}

// 独立朴素参考：每个输出样本后立即 erase 已消费前缀，直接照定义累加。
class ReferenceConverter {
 public:
  ReferenceConverter(std::uint32_t input_rate, std::uint32_t output_rate)
      : ratio_(static_cast<long double>(input_rate) /
               static_cast<long double>(output_rate)) {}

  void push(const float* samples, std::size_t count,
            std::vector<std::int16_t>& output) {
    input_.insert(input_.end(), samples, samples + count);
    total_input_ += count;
    drain(std::numeric_limits<std::size_t>::max(), output);
  }

  void flush(std::vector<std::int16_t>& output) {
    flushed_ = true;
    if (total_input_ == 0U) {
      return;
    }
    const long double exact =
        static_cast<long double>(total_input_) / ratio_;
    drain(static_cast<std::size_t>(std::floor(exact)), output);
  }

 private:
  bool can_emit() const {
    if (next_output_ == std::numeric_limits<std::size_t>::max()) {
      return false;
    }
    const long double source = static_cast<long double>(next_output_) * ratio_;
    const std::size_t index = static_cast<std::size_t>(std::floor(source));
    if (index >= total_input_) {
      return false;
    }
    if (flushed_) {
      return true;
    }
    const long double fraction = source - static_cast<long double>(index);
    return fraction == 0.0L || index + 1 < total_input_;
  }

  float at(std::size_t global_index) const {
    if (global_index < base_) {
      return 0.0F;
    }
    const std::size_t local = global_index - base_;
    if (local >= input_.size()) {
      return input_.empty() ? 0.0F : input_.back();
    }
    return input_[local];
  }

  void drain(std::size_t target, std::vector<std::int16_t>& output) {
    while (next_output_ < target && can_emit()) {
      const long double source = static_cast<long double>(next_output_) * ratio_;
      const std::size_t index = static_cast<std::size_t>(std::floor(source));
      const float fraction = static_cast<float>(
          source - static_cast<long double>(index));
      const float first = at(index);
      const float second = at(index + 1);
      output.push_back(to_s16(first + (second - first) * fraction));
      ++next_output_;
      const std::size_t drop = index - base_;
      if (drop > 0U) {
        if (drop > input_.size()) {
          input_.clear();
          base_ = total_input_;
        } else {
          input_.erase(input_.begin(),
                       input_.begin() + static_cast<std::ptrdiff_t>(drop));
          base_ = index;
        }
      }
    }
  }

  long double ratio_ = 0.0L;
  std::vector<float> input_;
  std::size_t base_ = 0U;
  std::size_t total_input_ = 0U;
  std::size_t next_output_ = 0U;
  bool flushed_ = false;
};

// 确定性输入：LCG 产生的 [-1,1) float，避免依赖随机库实现。
std::vector<float> make_input(std::size_t count) {
  std::vector<float> samples(count);
  std::uint32_t state = 0x12345678U;
  for (float& sample : samples) {
    state = state * 1664525U + 1013904223U;
    sample = static_cast<float>(state >> 8) / 8388608.0F - 1.0F;
  }
  return samples;
}

int failures = 0;

void expect(bool condition, const char* what) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

void run_chunked(const std::vector<float>& input, std::size_t chunk,
                 std::vector<std::int16_t>& output) {
  melo::MeloTtsPcmConverter converter(44100U, 16000U);
  for (std::size_t offset = 0; offset < input.size(); offset += chunk) {
    const std::size_t count = std::min(chunk, input.size() - offset);
    converter.push(input.data() + offset, count, output);
  }
  converter.flush(output);
}

}  // namespace

int main() {
  const std::vector<float> input = make_input(200000U);

  std::vector<std::int16_t> one_shot;
  run_chunked(input, input.size(), one_shot);

  std::vector<std::int16_t> reference;
  {
    ReferenceConverter converter(44100U, 16000U);
    converter.push(input.data(), input.size(), reference);
    converter.flush(reference);
  }
  expect(one_shot == reference, "一次性 push 与朴素参考逐样本一致");

  for (const std::size_t chunk : {7919U, 16384U, 32768U, 65536U}) {
    std::vector<std::int16_t> chunked;
    run_chunked(input, chunk, chunked);
    expect(chunked == one_shot, "分块 push 与一次性 push 逐样本一致");
    expect(chunked == reference, "分块 push 与朴素参考逐样本一致");
  }

  if (failures == 0) {
    std::printf("melotts_pcm_converter_test 通过 samples=%zu\n", one_shot.size());
    return 0;
  }
  std::fprintf(stderr, "melotts_pcm_converter_test 失败 %d 项\n", failures);
  return 1;
}
