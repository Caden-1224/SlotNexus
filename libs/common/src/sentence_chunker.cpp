#include "voxorchestra/common/sentence_chunker.hpp"

#include <cstddef>
#include <utility>

namespace voxorchestra::common {

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

}  // namespace

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
    out.push_back(cur_);
    cur_.clear();
    split_pending_ = true;
  };

  std::size_t i = 0;
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (is_cjk_terminator(text, i)) {
      on_terminator(text.substr(i, 3));
      i += 3;
      continue;
    }
    if (c == '!' || c == '?' || c == ';' || c == '\n') {
      on_terminator(c == '\n' ? std::string() : text.substr(i, 1));
      i += 1;
      continue;
    }
    if (c == '\r') {
      i += 1;  // CR 直接忽略（配合 \r\n）
      continue;
    }
    cur_.push_back(text[i]);
    split_pending_ = false;
    i += 1;
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

}  // namespace voxorchestra::common
