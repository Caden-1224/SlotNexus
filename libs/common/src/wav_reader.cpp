#include "voxorchestra/common/wav_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace voxorchestra::common {

namespace {

// 读取小端 32 位整数（字节序不敏感实现）。
std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t off) {
  return static_cast<std::uint32_t>(b[off]) |
         (static_cast<std::uint32_t>(b[off + 1]) << 8) |
         (static_cast<std::uint32_t>(b[off + 2]) << 16) |
         (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

std::uint16_t le16(const std::vector<std::uint8_t>& b, std::size_t off) {
  return static_cast<std::uint16_t>(b[off]) |
         (static_cast<std::uint16_t>(b[off + 1]) << 8);
}

bool tag_is(const std::vector<std::uint8_t>& b, std::size_t off,
            const char* tag) {
  return std::strncmp(reinterpret_cast<const char*>(b.data()) + off, tag, 4) ==
         0;
}

}  // namespace

WavReadResult WavReader::read(const std::string& path) {
  WavReadResult result;

  // 1. 读取整个文件（固定输入文件都很小；大文件解析由调用方分批处理）。
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    result.error = "无法打开文件: " + path;
    return result;
  }
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
  if (bytes.size() < 12) {
    result.error = "文件过短（不足 RIFF 头 12 字节）";
    return result;
  }

  // 2. RIFF 头：'RIFF' + 长度 + 'WAVE'。
  if (!tag_is(bytes, 0, "RIFF")) {
    result.error = "不是 RIFF 文件";
    return result;
  }
  if (!tag_is(bytes, 8, "WAVE")) {
    result.error = "RIFF 文件不是 WAVE 格式";
    return result;
  }

  // 3. 遍历块：fmt 必须存在且位于 data 之前；未知块按长度跳过。
  std::size_t pos = 12;
  bool have_fmt = false;
  bool have_data = false;
  std::size_t data_off = 0;
  std::size_t data_len = 0;

  while (pos + 8 <= bytes.size()) {
    const std::size_t chunk_off = pos + 8;
    const std::uint32_t chunk_len = le32(bytes, pos + 4);
    if (chunk_off + chunk_len > bytes.size()) {
      result.error = "块长度超出文件范围（文件可能被截断）";
      return result;
    }
    if (tag_is(bytes, pos, "fmt ")) {
      if (have_fmt) {
        result.error = "存在多个 fmt 块";
        return result;
      }
      if (chunk_len < 16) {
        result.error = "fmt 块长度不足（<16 字节）";
        return result;
      }
      const auto audio_format = le16(bytes, chunk_off);
      if (audio_format != 1) {
        result.error = "仅支持未压缩 PCM（format=1），实际 " +
                       std::to_string(audio_format);
        return result;
      }
      result.info.channels = le16(bytes, chunk_off + 2);
      result.info.sample_rate = le32(bytes, chunk_off + 4);
      result.info.bits = le16(bytes, chunk_off + 14);
      if (result.info.bits != 16) {
        result.error = "仅支持 16-bit 采样，实际 " +
                       std::to_string(result.info.bits);
        return result;
      }
      if (result.info.channels == 0 || result.info.sample_rate == 0) {
        result.error = "fmt 块声道数或采样率为 0";
        return result;
      }
      have_fmt = true;
    } else if (tag_is(bytes, pos, "data")) {
      if (!have_fmt) {
        result.error = "data 块出现在 fmt 块之前";
        return result;
      }
      data_off = chunk_off;
      data_len = chunk_len;
      have_data = true;
      break;  // data 之后不再需要解析
    }
    pos = chunk_off + chunk_len + (chunk_len % 2);  // 块按 2 字节对齐
  }

  if (!have_fmt) {
    result.error = "缺少 fmt 块";
    return result;
  }
  if (!have_data) {
    result.error = "缺少 data 块";
    return result;
  }

  // 4. 采样数量：16-bit 单声道按 2 字节一个采样；奇数字节数向下取整。
  const std::size_t sample_bytes = data_len - (data_len % 2);
  const std::size_t sample_count = sample_bytes / 2;
  result.info.samples.reserve(sample_count);
  for (std::size_t i = 0; i < sample_count; ++i) {
    const std::size_t off = data_off + i * 2;
    result.info.samples.push_back(static_cast<std::int16_t>(le16(bytes, off)));
  }

  result.ok = true;
  return result;
}

}  // namespace voxorchestra::common
