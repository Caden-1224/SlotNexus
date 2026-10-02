// MeloTTS 原生 float PCM → 契约 16 kHz S16_LE 的流式线性重采样。
// Author: Caden
//
// 线性重采样语义：分块送入与一次性送入得到逐样本一致的结果，跨块只保存
// 尚未消费的输入样本与下一个输出样本序号；不负责切 320 采样帧，也不补零。
#include "melotts_internal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace slotnexus::backend::melotts {
namespace {

// S16 满量程转换：[-1,1] 映射到 [-32768,32767]，越界样本先裁剪。
std::int16_t float_to_s16(float sample) noexcept {
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

}  // namespace

MeloTtsPcmConverter::MeloTtsPcmConverter(std::uint32_t input_rate_hz,
                                         std::uint32_t output_rate_hz)
    : input_rate_hz_(input_rate_hz),
      output_rate_hz_(output_rate_hz),
      ratio_(output_rate_hz == 0 ? 0.0
                                 : static_cast<double>(input_rate_hz) /
                                       static_cast<double>(output_rate_hz)) {}

bool MeloTtsPcmConverter::can_emit_next() const noexcept {
  if (!valid() || next_output_index_ == std::numeric_limits<std::size_t>::max()) {
    return false;
  }
  const long double source =
      static_cast<long double>(next_output_index_) * static_cast<long double>(ratio_);
  const std::size_t index = static_cast<std::size_t>(std::floor(source));
  if (index >= total_input_samples_) {
    return false;
  }
  if (flushed_) {
    // flush 的目标样本数已经由输入时长计算出来；最后一个输出区间可能落在
    // 最后两个输入样本之间，此时 sample_at 对越界的第二个点使用末尾样本
    // （零阶保持），避免末尾时间区间被截掉。
    return true;
  }
  const long double fraction = source - static_cast<long double>(index);
  if (fraction == 0.0L) {
    return true;
  }
  return index + 1 < total_input_samples_;
}

float MeloTtsPcmConverter::sample_at(std::size_t global_index) const noexcept {
  if (global_index < input_base_) {
    return 0.0F;
  }
  const std::size_t local_index = global_index - input_base_;
  if (local_index >= input_.size()) {
    return input_.empty() ? 0.0F : input_.back();
  }
  return input_[local_index];
}

void MeloTtsPcmConverter::compact_before(std::size_t global_index) {
  if (global_index <= input_base_) {
    return;
  }
  const std::size_t drop_count = global_index - input_base_;
  if (drop_count > input_.size()) {
    input_.clear();
    input_base_ = total_input_samples_;
    return;
  }
  // drain 每个输出样本都会推进到这里。若每次都 erase 已消费前缀，需要移动
  // 整个剩余输入，单次 push 就会退化成 O(输入样本×输出样本)。已消费样本留在
  // 缓冲里不参与计算，只有前缀超过一半时才搬移一次，摊还 O(1) 且输出逐样本不变。
  if (drop_count * 2U < input_.size()) {
    return;
  }
  input_.erase(input_.begin(),
               input_.begin() + static_cast<std::ptrdiff_t>(drop_count));
  input_base_ = global_index;
}

void MeloTtsPcmConverter::drain(std::size_t target,
                                std::vector<std::int16_t>& output) {
  if (!valid()) {
    return;
  }
  while (next_output_index_ < target && can_emit_next()) {
    const long double source =
        static_cast<long double>(next_output_index_) * static_cast<long double>(ratio_);
    const std::size_t index = static_cast<std::size_t>(std::floor(source));
    const float fraction = static_cast<float>(source - static_cast<long double>(index));
    const float first = sample_at(index);
    const float second = sample_at(index + 1);
    output.push_back(float_to_s16(first + (second - first) * fraction));
    ++next_output_index_;
    compact_before(index);
  }
}

void MeloTtsPcmConverter::push(const float* samples, std::size_t count,
                               std::vector<std::int16_t>& output) {
  if (!valid() || samples == nullptr || count == 0 || flushed_) {
    return;
  }
  input_.insert(input_.end(), samples, samples + count);
  total_input_samples_ += count;
  // 流式阶段不预知最终目标数量，只输出已经能完整插值的样本；flush 再补尾。
  drain(std::numeric_limits<std::size_t>::max(), output);
}

void MeloTtsPcmConverter::flush(std::vector<std::int16_t>& output) {
  if (!valid() || flushed_) {
    return;
  }
  flushed_ = true;
  const long double ratio = static_cast<long double>(input_rate_hz_) /
                            static_cast<long double>(output_rate_hz_);
  if (total_input_samples_ == 0) {
    return;
  }
  const long double exact_output = static_cast<long double>(total_input_samples_) / ratio;
  const std::size_t target = static_cast<std::size_t>(std::floor(exact_output));
  drain(target, output);
  input_.clear();
  input_base_ = total_input_samples_;
}

}  // namespace slotnexus::backend::melotts
