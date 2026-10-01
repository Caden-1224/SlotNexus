#include "slotnexus/common/sentence_chunker.hpp"
// Author: Caden

#include <cstddef>
#include <utility>

namespace slotnexus::common {

namespace {

// 中文句末标点的 UTF-8 三字节序列。
bool is_cjk_terminator(const std::string& s, std::size_t i) {
  if (i + 2 >= s.size()) {
    return false;
  }
  const auto b0 = static_cast<unsigned char>(s[i]);
  const auto b1 = static_cast<unsigned char>(s[i + 1]);
  const auto b2 = static_cast<unsigned char>(s[i + 2]);
  if (b0 == 0xE3 && b1 == 0x80) {
    return b2 == 0x82;  // 。
  }
  if (b0 == 0xEF && b1 == 0xBC) {
    return b2 == 0x81 || b2 == 0x9F || b2 == 0x9B;  // ！ ？ ；
  }
  return false;
}

// 宽松的 UTF-8 字符宽度推导：只用于容量切分时不切在字符中间。
std::size_t utf8_width(unsigned char lead) {
  if (lead < 0x80) {
    return 1;
  }
  if ((lead & 0xE0) == 0xC0) {
    return 2;
  }
  if ((lead & 0xF0) == 0xE0) {
    return 3;
  }
  if ((lead & 0xF8) == 0xF0) {
    return 4;
  }
  return 1;  // 非法前导字节按单字节推进，由上层收尾校验处理。
}

}  // namespace

SentenceChunker::SentenceChunker(std::size_t max_bytes,
                                 std::size_t first_max_bytes)
    : max_bytes_(max_bytes), first_max_bytes_(first_max_bytes) {}

std::size_t SentenceChunker::active_max_bytes() const {
  if (max_bytes_ == 0) {
    return 0;  // 旧行为：只按句末标点切分，首片软上限不参与。
  }
  if (!first_chunk_ || first_max_bytes_ == 0) {
    return max_bytes_;
  }
  return first_max_bytes_ < max_bytes_ ? first_max_bytes_ : max_bytes_;
}

std::vector<std::string> SentenceChunker::feed(const std::string& text) {
  std::vector<std::string> out;

  // 上一次 feed 可能在 UTF-8 字符中间结束。先把残留字节与本次输入拼成
  // 完整字符再扫描，避免容量切分落在字符内部，也保证跨 feed 的多字节
  // 句末标点仍按标点处理。
  std::string combined;
  const std::string* source = &text;
  if (!pending_utf8_.empty()) {
    combined = std::move(pending_utf8_);
    pending_utf8_.clear();
    combined += text;
    source = &combined;
  }
  const std::string& input = *source;
  const std::size_t n = input.size();

  // 处理一个切分点：punct 为归属前一句的标点（换行为空串，只切分不附标点）。
  // 连续句末标点只保留首个；句子不以标点开头（句首标点丢弃）。
  auto on_terminator = [&](const std::string& punct) {
    if (split_pending_ || cur_.empty()) {
      return;
    }
    cur_ += punct;
    out.push_back(std::move(cur_));
    cur_.clear();
    split_pending_ = true;
    first_chunk_ = false;
  };

  std::size_t i = 0;
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(input[i]);
    if (c == '\r') {
      ++i;  // CR 直接忽略（配合 \r\n）
      continue;
    }
    if (is_cjk_terminator(input, i)) {
      on_terminator(input.substr(i, 3));
      i += 3;
      continue;
    }
    if (c == '!' || c == '?' || c == ';' || c == '\n') {
      on_terminator(c == '\n' ? std::string() : input.substr(i, 1));
      ++i;
      continue;
    }

    const std::size_t width = utf8_width(c);
    if (i + width > n) {
      // 本次 feed 停在字符中间：暂存尾部字节，等下一段补齐后再按完整
      // 字符处理。这是 token 流边界，不代表文本已经截断。
      pending_utf8_.assign(input, i, n - i);
      split_pending_ = false;
      break;
    }
    const std::size_t limit = active_max_bytes();
    if (limit > 0 && !cur_.empty() && cur_.size() + width > limit) {
      // 普通字符到达当前片上限：先交付已有片段，再放当前字符。标点仍
      // 优先作为自然句末，不在这里拆句。
      out.push_back(std::move(cur_));
      cur_.clear();
      split_pending_ = false;
      first_chunk_ = false;
    }
    cur_.append(input, i, width);
    split_pending_ = false;
    i += width;
    if (first_chunk_ && first_max_bytes_ > 0 && max_bytes_ > 0 &&
        cur_.size() >= limit) {
      // 首片软上限达到即交付，不等下一个字符触发，减少首段进入 TTS 的
      // 等待；后续片段仍按普通 max_bytes 的原有触发方式切分。
      out.push_back(std::move(cur_));
      cur_.clear();
      split_pending_ = false;
      first_chunk_ = false;
    }
  }
  return out;
}

std::vector<std::string> SentenceChunker::flush() {
  std::vector<std::string> out;
  // flush 表示输入结束：尾部残留字节按原样收尾，保持确定性。
  if (!pending_utf8_.empty()) {
    cur_ += pending_utf8_;
    pending_utf8_.clear();
  }
  if (split_pending_) {
    // 上一句已切分，缓冲中只有（如果有的话）被并入开头的多余标点：丢弃。
    cur_.clear();
  } else if (!cur_.empty()) {
    out.push_back(std::move(cur_));
    cur_.clear();
  }
  // flush 后首片状态重置，下一轮 feed 重新使用 first_max_bytes。
  split_pending_ = false;
  first_chunk_ = true;
  return out;
}

}  // namespace slotnexus::common
