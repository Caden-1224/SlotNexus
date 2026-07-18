// FakeTtsBackend 单元测试：确定性 PCM 块序列、取消语义与会话重置。
#include "voxorchestra/backend/backend_event.hpp"
#include "voxorchestra/backend/fake/fake_tts_backend.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

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

// 收集一次合成会话的全部事件。
std::vector<eb::BackendEvent> collect(ef::FakeTtsBackend& tts) {
  std::vector<eb::BackendEvent> events;
  tts.set_event_callback([&events](const eb::BackendEvent& e) { events.push_back(e); });
  return events;
}

// 短文本（≤32 字节）：1 个 PCM 块 + kDone，采样值公式可逐点断言。
void test_short_text_single_chunk() {
  ef::FakeTtsBackend tts;
  auto events = collect(tts);
  tts.synthesize("你好");  // 6 字节 → ⌈6/32⌉ = 1 块

  CHECK(events.size() == 2);
  CHECK(events[0].kind == eb::BackendEvent::Kind::kPcm);
  CHECK(events[0].text.empty());
  CHECK(events[0].pcm.size() == static_cast<std::size_t>(eb::kFrameSamples));
  // 500 Hz 方波：每 32 采样一周期，前 16 采样 +6000、后 16 采样 -6000。
  CHECK(events[0].pcm[0] == 6000);
  CHECK(events[0].pcm[15] == 6000);
  CHECK(events[0].pcm[16] == -6000);
  CHECK(events[0].pcm[31] == -6000);
  CHECK(events[1].kind == eb::BackendEvent::Kind::kDone);
  CHECK(events[1].pcm.empty());
  std::cout << "  [ok] 短文本：单 PCM 块（320 采样）+ kDone，500 Hz 方波可断言"
            << std::endl;
}

// 长文本（>32 字节）：块数 = ⌈字节数/32⌉，相位跨块连续。
void test_long_text_multi_chunk() {
  ef::FakeTtsBackend tts;
  auto events = collect(tts);
  tts.synthesize(std::string(40, 'a'));  // 40 字节 → 2 块

  CHECK(events.size() == 3);
  CHECK(events[0].kind == eb::BackendEvent::Kind::kPcm);
  CHECK(events[1].kind == eb::BackendEvent::Kind::kPcm);
  CHECK(events[2].kind == eb::BackendEvent::Kind::kDone);
  CHECK(events[0].pcm.size() == static_cast<std::size_t>(eb::kFrameSamples));
  CHECK(events[1].pcm.size() == static_cast<std::size_t>(eb::kFrameSamples));
  CHECK(events[0].pcm[0] == 6000);  // 全局序号 0
  // 第二块首采样：全局序号 320，320 % 32 == 0 → 高电平（相位连续）。
  CHECK(events[1].pcm[0] == 6000);
  CHECK(events[1].pcm[15] == 6000);
  std::cout << "  [ok] 长文本：2 个 PCM 块，相位跨块连续（无跳变）" << std::endl;
}

// 空文本：仍产出 1 个 PCM 块 + kDone（消费者可依赖"必有输出与结束"）。
void test_empty_text() {
  ef::FakeTtsBackend tts;
  auto events = collect(tts);
  tts.synthesize("");
  CHECK(events.size() == 2);
  CHECK(events[0].kind == eb::BackendEvent::Kind::kPcm);
  CHECK(events[1].kind == eb::BackendEvent::Kind::kDone);
  std::cout << "  [ok] 空文本：单 PCM 块 + kDone" << std::endl;
}

// 取消：cancel 后 synthesize 不产出任何事件（含 kDone）。
void test_cancel_suppresses_synthesis() {
  ef::FakeTtsBackend tts;
  auto events = collect(tts);
  tts.cancel();
  tts.synthesize("你好");
  CHECK(events.empty());
  std::cout << "  [ok] 取消：cancel 后 synthesize 无任何事件" << std::endl;
}

// 会话重置：新 set_event_callback 清除取消状态，新会话正常合成。
void test_session_reset_clears_cancel() {
  ef::FakeTtsBackend tts;
  {
    auto events = collect(tts);
    tts.cancel();
    tts.synthesize("旧文本");
    CHECK(events.empty());
  }
  {
    auto events = collect(tts);  // 新会话
    tts.synthesize("新文本");
    CHECK(events.size() == 2);
    CHECK(events.back().kind == eb::BackendEvent::Kind::kDone);
  }
  std::cout << "  [ok] 会话重置：新会话不受上次取消影响" << std::endl;
}

// 确定性：相同文本两次合成，PCM 逐采样一致。
void test_determinism() {
  ef::FakeTtsBackend tts;
  std::vector<eb::BackendEvent> a;
  std::vector<eb::BackendEvent> b;
  tts.set_event_callback([&a](const eb::BackendEvent& e) { a.push_back(e); });
  tts.synthesize("确定性 测试");
  tts.set_event_callback([&b](const eb::BackendEvent& e) { b.push_back(e); });
  tts.synthesize("确定性 测试");

  CHECK(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    CHECK(a[i].kind == b[i].kind);
    CHECK(a[i].pcm == b[i].pcm);
  }
  std::cout << "  [ok] 确定性：同文本两次合成 PCM 逐采样一致" << std::endl;
}

}  // namespace

int main() {
  std::cout << "fake_tts_test:" << std::endl;
  test_short_text_single_chunk();
  test_long_text_multi_chunk();
  test_empty_text();
  test_cancel_suppresses_synthesis();
  test_session_reset_clears_cancel();
  test_determinism();

  if (g_failures == 0) {
    std::cout << "fake_tts_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "fake_tts_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
