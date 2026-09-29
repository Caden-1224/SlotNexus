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

SentenceChunker::SentenceChunker(std::size_t max_bytes)
    : max_bytes_(max_bytes) {}

std::vector<std::string> SentenceChunker::feed(const std::string& text) {
  std::vector<std::string> out;
  const std::size_t n = text.size();

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
  };

  std::size_t i = 0;
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == '\r') {
      ++i;  // CR 直接忽略（配合 \r\n）
      continue;
    }
    if (is_cjk_terminator(text, i)) {
      on_terminator(text.substr(i, 3));
      i += 3;
      continue;
    }
    if (c == '!' || c == '?' || c == ';' || c == '\n') {
      on_terminator(c == '\n' ? std::string() : text.substr(i, 1));
      ++i;
      continue;
    }

    const std::size_t width = utf8_width(c);
    if (i + width > n) {
      // 本次 feed 停在字符中间：先收下已有字节，等下一段补齐再切。
      // 这是 token 流边界，不代表文本已经截断。
      cur_.append(text, i, n - i);
      split_pending_ = false;
      break;
    }
    if (max_bytes_ > 0 && !cur_.empty() &&
        cur_.size() + width > max_bytes_) {
      // 普通字符到达上限：先交付已有片段，再放当前字符。标点仍优先作为
      // 自然句末，不在这里拆句；因此硬上限对普通字符生效。
      out.push_back(std::move(cur_));
      cur_.clear();
      split_pending_ = false;
    }
    cur_.append(text, i, width);
    split_pending_ = false;
    i += width;
  }
  return out;
}

std::vector<std::string> SentenceChunker::flush() {
  std::vector<std::string> out;
  if (split_pending_) {
    // 上一句已切分，缓冲中只有（如果有的话）被并入开头的多余标点：丢弃。
    cur_.clear();
    split_pending_ = false;
    return out;
  }
  if (!cur_.empty()) {
    out.push_back(std::move(cur_));
    cur_.clear();
  }
  return out;
}

}  // namespace slotnexus::common
