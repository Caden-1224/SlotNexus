// FakeAudioSink 单元测试：WAV 文件头、PCM 数据往返与生命周期语义。
#include "voxorchestra/backend/fake/fake_audio_sink.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

namespace eb = voxorchestra::backend;
namespace ef = voxorchestra::backend::fake;

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

// 测试用唯一临时路径（/tmp 下按 pid 区分，避免并发测试冲突）。
std::string TempPath(const std::string& name) {
  return (std::filesystem::temp_directory_path() /
          (name + "_" + std::to_string(::getpid()) + ".wav"))
      .string();
}

std::vector<std::uint8_t> ReadFile(const std::string& path) {
  std::vector<std::uint8_t> bytes;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    return bytes;
  }
  std::uint8_t buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    bytes.insert(bytes.end(), buf, buf + n);
  }
  std::fclose(f);
  return bytes;
}

std::uint32_t Le32(const std::vector<std::uint8_t>& b, std::size_t off) {
  return static_cast<std::uint32_t>(b[off]) |
         (static_cast<std::uint32_t>(b[off + 1]) << 8) |
         (static_cast<std::uint32_t>(b[off + 2]) << 16) |
         (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

std::uint16_t Le16(const std::vector<std::uint8_t>& b, std::size_t off) {
  return static_cast<std::uint16_t>(b[off] | (b[off + 1] << 8));
}

// 完整往返：open → write 两帧 → close，验证 44 字节头字段与 PCM 内容逐采样一致。
void test_header_and_data_roundtrip() {
  const std::string path = TempPath("ef_sink_roundtrip");
  const std::vector<int16_t> chunk1(320, 100);
  const std::vector<int16_t> chunk2(320, -200);

  {
    ef::FakeAudioSink sink(path);
    CHECK(sink.open());
    CHECK(sink.write_pcm(chunk1));
    CHECK(sink.write_pcm(chunk2));
    CHECK(sink.close());
  }
  const auto b = ReadFile(path);
  std::remove(path.c_str());

  // 44 字节头 + 2×320×2 字节数据。
  CHECK(b.size() == 44 + 1280);
  CHECK(std::string(b.begin(), b.begin() + 4) == "RIFF");
  CHECK(Le32(b, 4) == 36 + 1280);            // RIFF 块长
  CHECK(std::string(b.begin() + 8, b.begin() + 12) == "WAVE");
  CHECK(std::string(b.begin() + 12, b.begin() + 16) == "fmt ");
  CHECK(Le32(b, 16) == 16);                  // fmt 块长
  CHECK(Le16(b, 20) == 1);                   // PCM
  CHECK(Le16(b, 22) == 1);                   // 单声道
  CHECK(Le32(b, 24) == static_cast<std::uint32_t>(eb::kSampleRateHz));
  CHECK(Le32(b, 28) == 2 * static_cast<std::uint32_t>(eb::kSampleRateHz));  // 字节率
  CHECK(Le16(b, 32) == 2);                   // 块对齐
  CHECK(Le16(b, 34) == 16);                  // 位深
  CHECK(std::string(b.begin() + 36, b.begin() + 40) == "data");
  CHECK(Le32(b, 40) == 1280);                // 数据字节数
  // PCM 数据逐采样一致（小端 int16）。
  for (std::size_t i = 0; i < 320; ++i) {
    CHECK(Le16(b, 44 + i * 2) == 100);
  }
  for (std::size_t i = 0; i < 320; ++i) {
    CHECK(Le16(b, 44 + 640 + i * 2) == static_cast<std::uint16_t>(-200));
  }
  std::cout << "  [ok] WAV 往返：44 字节头字段正确、PCM 逐采样一致" << std::endl;
}

// 生命周期：未 open 重复 open 返回 false；未 open write 返回 false；
// close 幂等（重复 close、未 open 均返回 true）。
void test_lifecycle_semantics() {
  const std::string path = TempPath("ef_sink_life");
  {
    ef::FakeAudioSink sink(path);
    CHECK(!sink.write_pcm({1, 2, 3}));   // 未 open 拒绝写入
    CHECK(sink.close());                 // 未 open close 为空操作
    CHECK(sink.open());
    CHECK(!sink.open());                 // 重复 open 拒绝
    CHECK(sink.write_pcm({1, 2, 3}));
    CHECK(sink.close());
    CHECK(sink.close());                 // 重复 close 幂等
    CHECK(sink.open());                  // 关闭后可再次 open（重写文件）
    CHECK(sink.close());
  }
  std::remove(path.c_str());
  std::cout << "  [ok] 生命周期：重复 open 拒绝、未 open 拒绝写入、close 幂等" << std::endl;
}

// 析构自动 close：未显式 close 也生成完整 WAV（头已回填）。
void test_destructor_auto_close() {
  const std::string path = TempPath("ef_sink_auto");
  {
    ef::FakeAudioSink sink(path);
    CHECK(sink.open());
    CHECK(sink.write_pcm(std::vector<int16_t>(320, 7)));
    // 不调用 close，依赖析构。
  }
  const auto b = ReadFile(path);
  std::remove(path.c_str());
  CHECK(b.size() == 44 + 640);
  CHECK(Le32(b, 40) == 640);  // 头已回填
  std::cout << "  [ok] 析构自动 close：异常路径也生成完整 WAV" << std::endl;
}

}  // namespace

int main() {
  std::cout << "fake_audio_sink_test:" << std::endl;
  test_header_and_data_roundtrip();
  test_lifecycle_semantics();
  test_destructor_auto_close();

  if (g_failures == 0) {
    std::cout << "fake_audio_sink_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "fake_audio_sink_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
