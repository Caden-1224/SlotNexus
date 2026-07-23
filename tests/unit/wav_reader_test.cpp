// WavReader 单元测试：标准 16-bit PCM 解析、未知块跳过、格式拒绝与损坏输入。
#include "voxorchestra/common/wav_reader.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>  // getpid（临时文件唯一名）

namespace cq = voxorchestra::common;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #cond   \
                << std::endl;                                                \
    }                                                                        \
  } while (0)

// 手工构造 RIFF/WAVE 文件字节（测试不依赖第三方写 WAV 的代码）。
std::vector<std::uint8_t> build_wav(
    const std::vector<std::int16_t>& samples, std::uint32_t sample_rate = 16000,
    std::uint16_t channels = 1, std::uint16_t bits = 16,
    std::uint16_t format = 1,
    const std::vector<std::string>& extra_chunks = {}) {
  std::vector<std::uint8_t> b;
  auto push_le16 = [&](std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
  };
  auto push_le32 = [&](std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v >> 16));
    b.push_back(static_cast<std::uint8_t>(v >> 24));
  };
  auto push_tag = [&](const char* tag) {
    for (int i = 0; i < 4; ++i) {
      b.push_back(static_cast<std::uint8_t>(tag[i]));
    }
  };

  const std::uint32_t data_bytes =
      static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
  push_tag("RIFF");
  push_le32(36 + data_bytes);  // 占位，读取器不校验该值
  push_tag("WAVE");
  push_tag("fmt ");
  push_le32(16);
  push_le16(format);
  push_le16(channels);
  push_le32(sample_rate);
  push_le32(sample_rate * channels * bits / 8);
  push_le16(static_cast<std::uint16_t>(channels * bits / 8));
  push_le16(bits);
  for (const auto& chunk : extra_chunks) {
    push_tag(chunk.c_str());
    push_le32(4);
    for (int i = 0; i < 4; ++i) {
      b.push_back(static_cast<std::uint8_t>('x' + i));
    }
  }
  push_tag("data");
  push_le32(data_bytes);
  for (auto s : samples) {
    push_le16(static_cast<std::uint16_t>(s));
  }
  return b;
}

std::string tmp_wav_path(const std::string& name) {
  return std::filesystem::temp_directory_path().string() + "/vox_wav_" + name +
         "_" + std::to_string(::getpid()) + ".wav";
}

bool write_bytes(const std::string& path, const std::vector<std::uint8_t>& b) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out.write(reinterpret_cast<const char*>(b.data()),
            static_cast<std::streamsize>(b.size()));
  return out.good();
}

void test_happy_path() {
  const std::string path = tmp_wav_path("ok");
  // 640 采样 = 2 帧 × 320（16kHz 20ms）；采样值确定可断言。
  std::vector<std::int16_t> samples;
  for (int i = 0; i < 640; ++i) {
    samples.push_back(static_cast<std::int16_t>((i * 7) % 1000 - 500));
  }
  CHECK(write_bytes(path, build_wav(samples)));
  const auto r = cq::WavReader::read(path);
  CHECK(r.ok);
  if (r.ok) {
    CHECK(r.info.sample_rate == 16000);
    CHECK(r.info.channels == 1);
    CHECK(r.info.bits == 16);
    CHECK(r.info.samples.size() == samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
      if (r.info.samples[i] != samples[i]) {
        ++g_failures;
        std::cerr << "FAIL 采样[" << i << "] 期望 " << samples[i] << " 实际 "
                  << r.info.samples[i] << std::endl;
        break;
      }
    }
  }
  std::remove(path.c_str());
  std::cout << "  [ok] 标准 16-bit PCM：头信息与全部采样逐值一致" << std::endl;
}

void test_extra_chunks_skipped() {
  const std::string path = tmp_wav_path("extra");
  std::vector<std::int16_t> samples = {1, 2, 3};
  // LIST 与 fact 块位于 fmt 与 data 之间，应被跳过。
  CHECK(write_bytes(path, build_wav(samples, 16000, 1, 16, 1, {"LIST", "fact"})));
  const auto r = cq::WavReader::read(path);
  CHECK(r.ok);
  if (r.ok) {
    CHECK(r.info.samples == samples);
  }
  std::remove(path.c_str());
  std::cout << "  [ok] 未知块（LIST/fact）按块结构跳过" << std::endl;
}

void test_format_rejected() {
  const std::string path = tmp_wav_path("float");
  // IEEE float（format=3）不支持。
  CHECK(write_bytes(path, build_wav({1, 2}, 16000, 1, 32, 3)));
  auto r = cq::WavReader::read(path);
  CHECK(!r.ok);
  CHECK(r.error.find("PCM") != std::string::npos);
  std::remove(path.c_str());
  std::cout << "  [ok] 非 PCM 压缩格式明确拒绝" << std::endl;
}

void test_bits_rejected() {
  const std::string path = tmp_wav_path("8bit");
  CHECK(write_bytes(path, build_wav({1, 2}, 16000, 1, 8, 1)));
  const auto r = cq::WavReader::read(path);
  CHECK(!r.ok);
  CHECK(r.error.find("16-bit") != std::string::npos);
  std::remove(path.c_str());
  std::cout << "  [ok] 非 16-bit 采样明确拒绝" << std::endl;
}

void test_truncated_rejected() {
  const std::string path = tmp_wav_path("trunc");
  auto bytes = build_wav({1, 2, 3, 4}, 16000, 1, 16, 1);
  bytes.resize(bytes.size() - 3);  // 截断 data 尾部
  CHECK(write_bytes(path, bytes));
  const auto r = cq::WavReader::read(path);
  CHECK(!r.ok);
  std::remove(path.c_str());
  std::cout << "  [ok] 截断文件（data 超出文件范围）拒绝" << std::endl;
}

void test_missing_fmt_rejected() {
  const std::string path = tmp_wav_path("nofmt");
  // 直接构造：RIFF/WAVE + data，无 fmt。
  std::vector<std::uint8_t> b;
  const char* riff = "RIFF";
  const char* wave = "WAVE";
  const char* data = "data";
  b.insert(b.end(), riff, riff + 4);
  b.insert(b.end(), {0, 0, 0, 0});
  b.insert(b.end(), wave, wave + 4);
  b.insert(b.end(), data, data + 4);
  b.insert(b.end(), {4, 0, 0, 0, 0, 0, 0, 0});
  CHECK(write_bytes(path, b));
  const auto r = cq::WavReader::read(path);
  CHECK(!r.ok);
  CHECK(r.error.find("fmt") != std::string::npos);
  std::remove(path.c_str());
  std::cout << "  [ok] 缺少 fmt 块拒绝" << std::endl;
}

void test_missing_file() {
  const auto r = cq::WavReader::read(tmp_wav_path("missing"));
  CHECK(!r.ok);
  CHECK(r.error.find("无法打开") != std::string::npos);
  std::cout << "  [ok] 文件不存在：错误信息明确" << std::endl;
}

void test_odd_data_length() {
  const std::string path = tmp_wav_path("odd");
  // 手工构造 data 长度 5（奇数）：向下取整读取 2 个采样。
  std::vector<std::uint8_t> b = build_wav({100, 200}, 16000, 1, 16, 1);
  // 覆写 data 块长度字段（fmt 16 字节后紧接 data 标签）：
  // data 标签在 12+8+16=36 处，长度在 40 处。声明 5 字节，需补齐 1 字节
  // 使块长度不超出文件范围（超出按截断拒绝，属另一分支）。
  b[40] = 5;
  b[41] = 0;
  b[42] = 0;
  b[43] = 0;
  b.push_back(0xAA);  // 第 5 个 data 字节（填充）
  CHECK(write_bytes(path, b));
  const auto r = cq::WavReader::read(path);
  CHECK(r.ok);
  if (r.ok) {
    CHECK(r.info.samples.size() == 2);
    CHECK(r.info.samples[0] == 100);
  }
  std::remove(path.c_str());
  std::cout << "  [ok] 奇数 data 长度：向下取整不崩溃" << std::endl;
}

}  // namespace

int main() {
  std::cout << "wav_reader_test:" << std::endl;
  test_happy_path();
  test_extra_chunks_skipped();
  test_format_rejected();
  test_bits_rejected();
  test_truncated_rejected();
  test_missing_fmt_rejected();
  test_missing_file();
  test_odd_data_length();

  if (g_failures == 0) {
    std::cout << "wav_reader_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "wav_reader_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
