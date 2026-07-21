// NDJSON 增量解帧器单元测试：
// 半包/粘包/多帧/CRLF/空行/超长帧/逐字节喂入/UTF-8 跨块。
#include "voxorchestra/network/frame_decoder.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace en = voxorchestra::network;

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

void test_single_frame() {
  en::NdjsonFrameDecoder d;
  std::vector<std::string> frames;
  CHECK(d.feed(R"({"type":"event","index":0})" "\n", frames) == en::FrameResult::kOk);
  CHECK(frames.size() == 1);
  CHECK(frames[0] == R"({"type":"event","index":0})");
  std::cout << "  [ok] 单帧完整到达" << std::endl;
}

void test_split_frame() {
  // 半包：一帧被切成两段到达。
  const std::string full = R"({"type":"event","index":0})" "\n";
  for (std::size_t split = 1; split < full.size(); ++split) {
    en::NdjsonFrameDecoder d;
    std::vector<std::string> frames;
    CHECK(d.feed(std::string_view(full).substr(0, split), frames) == en::FrameResult::kOk);
    CHECK(d.feed(std::string_view(full).substr(split), frames) == en::FrameResult::kOk);
    CHECK(frames.size() == 1);
    CHECK(frames[0] == R"({"type":"event","index":0})");
  }
  std::cout << "  [ok] 半包：任意切分位置两次喂入均还原为一帧" << std::endl;
}

void test_byte_by_byte() {
  // 逐字节喂入 3 帧（含 UTF-8 中文，验证跨块不破坏字节）。
  const std::vector<std::string> expected = {
      R"({"type":"event","text":"你好"})",
      R"({"type":"event","text":"世界"})",
      R"({"type":"event","text":"ok"})",
  };
  std::string all;
  for (const auto& f : expected) {
    all += f + "\n";
  }
  en::NdjsonFrameDecoder d;
  std::vector<std::string> frames;
  for (char c : all) {
    CHECK(d.feed(std::string_view(&c, 1), frames) == en::FrameResult::kOk);
  }
  CHECK(frames.size() == expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    CHECK(frames[i] == expected[i]);
  }
  std::cout << "  [ok] 逐字节喂入 3 帧按序还原（UTF-8 中文跨块无损）" << std::endl;
}

void test_multiple_frames_one_chunk() {
  // 粘包：一个 chunk 里多帧 + 尾部半帧。
  en::NdjsonFrameDecoder d;
  std::vector<std::string> frames;
  const std::string chunk = R"({"type":"a"})" "\n" R"({"type":"b"})" "\n" R"({"type":"c"})" "\n" R"({"type":"d"})";
  CHECK(d.feed(chunk, frames) == en::FrameResult::kOk);
  CHECK(frames.size() == 3);
  CHECK(frames[0] == R"({"type":"a"})");
  CHECK(frames[1] == R"({"type":"b"})");
  CHECK(frames[2] == R"({"type":"c"})");
  CHECK(d.partial_size() == std::string(R"({"type":"d"})").size());
  // 补上剩余字节（换行）后第 4 帧完成。
  CHECK(d.feed("\n", frames) == en::FrameResult::kOk);
  CHECK(frames.size() == 4);
  CHECK(frames[3] == R"({"type":"d"})");
  CHECK(d.partial_size() == 0);
  std::cout << "  [ok] 粘包多帧 + 尾部半帧处理正确" << std::endl;
}

void test_crlf() {
  en::NdjsonFrameDecoder d;
  std::vector<std::string> frames;
  CHECK(d.feed(R"({"type":"a"})" "\r\n" R"({"type":"b"})" "\r\n", frames) == en::FrameResult::kOk);
  CHECK(frames.size() == 2);
  CHECK(frames[0] == R"({"type":"a"})");
  CHECK(frames[1] == R"({"type":"b"})");
  std::cout << "  [ok] CRLF 行尾兼容，\\r 被剥离" << std::endl;
}

void test_empty_lines_skipped() {
  en::NdjsonFrameDecoder d;
  std::vector<std::string> frames;
  const std::string chunk = "\n" R"({"type":"a"})" "\n\n\r\n" R"({"type":"b"})" "\n\n";
  CHECK(d.feed(chunk, frames) == en::FrameResult::kOk);
  CHECK(frames.size() == 2);
  CHECK(frames[0] == R"({"type":"a"})");
  CHECK(frames[1] == R"({"type":"b"})");
  std::cout << "  [ok] 空行与纯 CR 行被跳过" << std::endl;
}

void test_oversized() {
  // 无换行且超过上限 → kOversized，缓冲清空，reset 后可用。
  en::NdjsonFrameDecoder d(64);
  std::vector<std::string> frames;
  std::string big(65, 'x');
  CHECK(d.feed(big, frames) == en::FrameResult::kOversized);
  CHECK(d.partial_size() == 0);

  // 恰好等于上限（无换行）：允许继续等待换行。
  en::NdjsonFrameDecoder d2(64);
  CHECK(d2.feed(std::string(64, 'y'), frames) == en::FrameResult::kOk);
  CHECK(d2.partial_size() == 64);
  // 再来一个字节才超限。
  CHECK(d2.feed("z", frames) == en::FrameResult::kOversized);

  // reset 后解码器可复用。
  d2.reset();
  CHECK(d2.feed("ok\n", frames) == en::FrameResult::kOk);
  CHECK(frames.back() == "ok");
  std::cout << "  [ok] 超长帧报错并清空缓冲，reset 后可复用" << std::endl;
}

void test_oversized_with_newline() {
  // 带换行的超长帧同样必须拒绝（此前只在"无换行"分支检查上限，
  // 客户端可发送任意长的帧绕过 1 MiB 保护）。
  en::NdjsonFrameDecoder d(64);
  std::vector<std::string> frames;
  CHECK(d.feed(std::string(65, 'x') + "\n", frames) == en::FrameResult::kOversized);
  CHECK(frames.empty());
  CHECK(d.partial_size() == 0);

  // 分两次喂入、换行在第二次到达：仍须拒绝。
  en::NdjsonFrameDecoder d2(64);
  CHECK(d2.feed(std::string(60, 'x'), frames) == en::FrameResult::kOk);
  CHECK(d2.feed(std::string(10, 'x') + "\n", frames) == en::FrameResult::kOversized);
  CHECK(d2.partial_size() == 0);

  // CRLF 形式的超长帧也拒绝。
  en::NdjsonFrameDecoder d3(64);
  CHECK(d3.feed(std::string(65, 'x') + "\r\n", frames) == en::FrameResult::kOversized);
  CHECK(frames.empty());
  std::cout << "  [ok] 带换行/CRLF 的超长帧一律 kOversized" << std::endl;
}

void test_frame_exactly_at_limit() {
  // 帧内容恰好等于上限且带换行：合法。
  en::NdjsonFrameDecoder d(64);
  std::vector<std::string> frames;
  CHECK(d.feed(std::string(64, 'x') + "\n", frames) == en::FrameResult::kOk);
  CHECK(frames.size() == 1);
  CHECK(frames[0].size() == 64);
  std::cout << "  [ok] 帧长度恰好等于上限时合法" << std::endl;
}

}  // namespace

int main() {
  std::cout << "frame_decoder_test:" << std::endl;
  test_single_frame();
  test_split_frame();
  test_byte_by_byte();
  test_multiple_frames_one_chunk();
  test_crlf();
  test_empty_lines_skipped();
  test_oversized();
  test_oversized_with_newline();
  test_frame_exactly_at_limit();

  if (g_failures == 0) {
    std::cout << "frame_decoder_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "frame_decoder_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
