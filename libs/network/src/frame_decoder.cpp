#include "voxorchestra/network/frame_decoder.hpp"

#include <algorithm>
#include <utility>

namespace voxorchestra::network {

NdjsonFrameDecoder::NdjsonFrameDecoder(std::size_t max_frame_bytes)
    : max_frame_bytes_(max_frame_bytes) {}

FrameResult NdjsonFrameDecoder::feed(std::string_view chunk,
                                     std::vector<std::string>& frames) {
  buffer_.append(chunk.data(), chunk.size());

  while (true) {
    const std::size_t nl = buffer_.find('\n');
    if (nl == std::string::npos) {
      // 剩余内容还没有换行：超过上限即协议错误，否则等待后续字节。
      if (buffer_.size() > max_frame_bytes_) {
        reset();
        return FrameResult::kOversized;
      }
      return FrameResult::kOk;
    }

    // 帧内容为 [0, nl)，剥离行尾 '\r' 以兼容 CRLF。
    std::string frame(buffer_.data(), nl);
    if (!frame.empty() && frame.back() == '\r') {
      frame.pop_back();
    }
    // 无论是否已带换行，超过上限一律视为协议错误（否则超长帧
    // 可在换行命中后绕过限制）。
    if (frame.size() > max_frame_bytes_) {
      reset();
      return FrameResult::kOversized;
    }
    buffer_.erase(0, nl + 1);

    // 空白行不构成有效 JSON 帧，跳过。
    if (!frame.empty()) {
      frames.push_back(std::move(frame));
    }
  }
}

std::size_t NdjsonFrameDecoder::partial_size() const { return buffer_.size(); }

void NdjsonFrameDecoder::reset() { buffer_.clear(); }

}  // namespace voxorchestra::network
