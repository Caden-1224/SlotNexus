// 最小 base64 编解码（RFC 4648 标准表，无外部依赖）。
//
// 用于把二进制负载（如 16 kHz int16 PCM）编码进控制面 JSON 信封的
// text 字段上行（NetAsrBackend 音频上行负载约定）。与 dataplane 事件
// 的 PCM base64 编码同表实现（libs/dataplane 内部另有一份，保持独立，
// 不引入跨库耦合）。解码失败（非法字符）抛 std::invalid_argument。
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace voxorchestra::common {

namespace detail {

constexpr char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline int base64_char_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

}  // namespace detail

// 编码原始字节 → 标准 base64（含 '=' 填充）。
inline std::string base64_encode(const std::uint8_t* data, std::size_t n) {
  std::string out;
  out.reserve(((n + 2) / 3) * 4);
  for (std::size_t i = 0; i < n; i += 3) {
    const std::uint32_t b0 = data[i];
    const std::uint32_t b1 = (i + 1 < n) ? data[i + 1] : 0;
    const std::uint32_t b2 = (i + 2 < n) ? data[i + 2] : 0;
    const std::uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
    out.push_back(detail::kBase64Chars[(triple >> 18) & 0x3F]);
    out.push_back(detail::kBase64Chars[(triple >> 12) & 0x3F]);
    out.push_back(i + 1 < n ? detail::kBase64Chars[(triple >> 6) & 0x3F] : '=');
    out.push_back(i + 2 < n ? detail::kBase64Chars[triple & 0x3F] : '=');
  }
  return out;
}

// 解码标准 base64 → 原始字节。非法字符抛 std::invalid_argument。
inline std::vector<std::uint8_t> base64_decode(const std::string& s) {
  std::vector<std::uint8_t> out;
  out.reserve((s.size() / 4) * 3);
  int accum = 0;
  int bits = 0;
  for (const char c : s) {
    if (c == '=') {
      break;
    }
    const int v = detail::base64_char_value(c);
    if (v < 0) {
      throw std::invalid_argument("非法 base64 字符");
    }
    accum = (accum << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((accum >> bits) & 0xFF));
    }
  }
  return out;
}

}  // namespace voxorchestra::common
