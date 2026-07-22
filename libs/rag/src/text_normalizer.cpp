#include "voxorchestra/rag/text_normalizer.hpp"

#include <cstdint>
#include <cctype>

namespace voxorchestra::rag {

namespace {

// UTF-8 单码点解码；无效字节序列按逐字节降级处理（保持确定性，不抛异常）。
// 返回解码后的码点，并把 pos 前进到下一个字符的起始位置。
char32_t decode_utf8(const std::string& s, std::size_t& pos) {
  const auto lead = static_cast<std::uint8_t>(s[pos]);
  if (lead < 0x80) {
    pos += 1;
    return static_cast<char32_t>(lead);
  }
  int extra = 0;
  char32_t cp = 0;
  if ((lead & 0xE0) == 0xC0) {
    extra = 1;
    cp = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    extra = 2;
    cp = lead & 0x0F;
  } else if ((lead & 0xF8) == 0xF0) {
    extra = 3;
    cp = lead & 0x07;
  } else {
    pos += 1;  // 无效首字节：按单字节处理
    return static_cast<char32_t>(lead);
  }
  if (pos + extra >= s.size()) {
    pos = s.size();  // 截断的多字节序列：丢弃其余字节
    return 0;
  }
  for (int i = 1; i <= extra; ++i) {
    const auto b = static_cast<std::uint8_t>(s[pos + i]);
    if ((b & 0xC0) != 0x80) {
      pos += 1;  // 非法续字节：降级为单字节
      return static_cast<char32_t>(lead);
    }
    cp = (cp << 6) | (b & 0x3F);
  }
  pos += 1 + static_cast<std::size_t>(extra);
  return cp;
}

// 码点 → 规范化后字符的 UTF-8（用于重建规范化文本）。
void append_utf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// 全角（U+FF01..U+FF5E）折半角；全角空格 U+3000 折普通空格；其余原样。
char32_t fold_width(char32_t c) {
  if (c >= 0xFF01 && c <= 0xFF5E) {
    return c - 0xFEE0;
  }
  if (c == 0x3000) {
    return 0x20;
  }
  return c;
}

bool is_space(char32_t c) { return c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D; }

bool is_token_char(char32_t c) {
  if (c < 0x80) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
  }
  return is_cjk(c);
}

}  // namespace

std::string normalize_text(const std::string& text) {
  std::string out;
  std::size_t pos = 0;
  bool pending_space = false;  // 遇到空白先标记，仅在后续有内容时写出一个空格
  while (pos < text.size()) {
    const char32_t c = fold_width(decode_utf8(text, pos));
    if (is_space(c)) {
      if (!out.empty()) {
        pending_space = true;
      }
      continue;
    }
    if (!is_token_char(c)) {
      continue;  // 标点等符号丢弃
    }
    if (pending_space) {
      out.push_back(' ');
      pending_space = false;
    }
    const char32_t folded = (c < 0x80) ? static_cast<char32_t>(std::tolower(static_cast<unsigned char>(c))) : c;
    append_utf8(out, folded);
  }
  return out;
}

std::vector<std::string> tokenize(const std::string& text) {
  std::vector<std::string> tokens;
  std::string ascii_run;
  std::size_t pos = 0;
  while (pos < text.size()) {
    const char32_t c = fold_width(decode_utf8(text, pos));
    if (c < 0x80) {
      const char lower =
          static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
        ascii_run.push_back(lower);
        continue;
      }
      if (!ascii_run.empty()) {
        tokens.push_back(ascii_run);
        ascii_run.clear();
      }
      continue;
    }
    if (is_cjk(c)) {
      if (!ascii_run.empty()) {
        tokens.push_back(ascii_run);
        ascii_run.clear();
      }
      std::string t;
      append_utf8(t, c);
      tokens.push_back(t);
      continue;
    }
    // 非 CJK 符号：清空进行中的 ASCII run（符号是 token 边界）。
    if (!ascii_run.empty()) {
      tokens.push_back(ascii_run);
      ascii_run.clear();
    }
  }
  if (!ascii_run.empty()) {
    tokens.push_back(ascii_run);
  }
  return tokens;
}

}  // namespace voxorchestra::rag
