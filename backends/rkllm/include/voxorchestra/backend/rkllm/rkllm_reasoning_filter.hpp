// ReasoningFilter：把思考段（reasoning / thinking）从流式 token 中剔除。
//
// 背景：部分模型的 .rkllm 产物会在正式回答前输出一段思考内容，包在
// <think>…</think> 里。下游（TTS 朗读、
// 会话文本）只需要正式回答，思考过程必须丢弃；但标记可能跨 token 断开，
// 因此不能按单个 token 做字符串比较。
//
// 语义（与改造前硬编码的单一模型专用逻辑保持一致）：
//   - end_tag 为空 → 关闭过滤，accept() 原样返回，零开销直通；
//   - 启用后累积文本直到出现 end_tag；出现之前的增量一律不下发；
//   - end_tag 可能重复出现，取最后一次（保守：思考内容里出现字面量时不误切）；
//   - end_tag 之后的内容立即下发（可能为空）；此后所有增量直接下发；
//   - 缓冲超过 max_buffer_bytes 仍未见 end_tag → 判定不会闭合，把缓冲原样
//     放行并关闭过滤：既保证下游有内容可读，也保证内存有界；
//   - flush() 供会话结束（FINISH / ERROR）调用：思考段未闭合时回退放行，
//     避免 token 预算耗尽导致下游完全无输出。
//
// 约束：纯逻辑，不依赖厂商 SDK，默认构建即可单测（见
// tests/unit/rkllm_reasoning_filter_test.cpp）。
#pragma once

#include <cstddef>
#include <string>
#include <utility>

namespace voxorchestra::backend::rkllm {

class ReasoningFilter {
 public:
  ReasoningFilter() = default;

  explicit ReasoningFilter(std::string end_tag,
                           std::size_t max_buffer_bytes = 256u * 1024u)
      : end_tag_(std::move(end_tag)),
        max_buffer_bytes_(max_buffer_bytes > 0u ? max_buffer_bytes : 1u) {}

  // 是否启用过滤（end_tag 非空）。
  bool enabled() const noexcept { return !end_tag_.empty(); }

  // 过滤是否已结束（已见到闭合标记、已按上限放行、或已 flush）。
  // 结束后 accept() 只做直通。
  bool closed() const noexcept { return closed_; }

  // 输入一段增量文本，返回真正应下发给下游的文本（可能为空）。
  std::string accept(const std::string& delta) {
    if (!enabled() || closed_) {
      return delta;
    }
    buffer_ += delta;
    const std::size_t pos = buffer_.rfind(end_tag_);
    if (pos != std::string::npos) {
      closed_ = true;
      std::string out = buffer_.substr(pos + end_tag_.size());
      buffer_.clear();
      return out;
    }
    if (buffer_.size() > max_buffer_bytes_) {
      // 未闭合且已超上限：原样放行，避免无界增长（下游至少拿到内容）。
      closed_ = true;
      std::string out;
      out.swap(buffer_);
      return out;
    }
    return {};
  }

  // 会话结束：返回仍未下发的回退文本（未闭合的思考段原样放行）。
  std::string flush() {
    if (!enabled() || closed_) {
      return {};
    }
    closed_ = true;
    std::string out;
    out.swap(buffer_);
    return out;
  }

  // 只读诊断：当前缓冲字节数（证据与测试用，不参与业务分支）。
  std::size_t buffered_bytes() const noexcept { return buffer_.size(); }

 private:
  std::string end_tag_;
  std::string buffer_;
  std::size_t max_buffer_bytes_ = 256u * 1024u;
  bool closed_ = false;
};

}  // namespace voxorchestra::backend::rkllm
