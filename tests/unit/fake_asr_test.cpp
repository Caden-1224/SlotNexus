// FakeAsrBackend 单元测试：确定性事件序列、取消语义与会话重置。
//
// Fake 的确定性是协议与编排测试的前提：相同输入必须产出完全相同的
// partial/final 序列，且取消后不得再产出事件。
#include "voxorchestra/backend/backend_event.hpp"
#include "voxorchestra/backend/fake/fake_asr_backend.hpp"
#include "voxorchestra/backend/fake/fake_audio_source.hpp"

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

// 收集一次会话的全部事件。
std::vector<eb::BackendEvent> collect(ef::FakeAsrBackend& asr) {
  std::vector<eb::BackendEvent> events;
  asr.set_event_callback([&events](const eb::BackendEvent& e) { events.push_back(e); });
  return events;
}

// 确定性：3 帧（20ms 帧）→ 3 条 partial + 1 条 final，文本可精确断言。
void test_deterministic_partial_final() {
  ef::FakeAsrBackend asr;
  auto events = collect(asr);
  asr.feed_audio(ef::FakeAudioSource::make_frame(0), false);
  asr.feed_audio(ef::FakeAudioSource::make_frame(1), false);
  asr.feed_audio(ef::FakeAudioSource::make_frame(2), true);

  CHECK(events.size() == 4);
  CHECK(events[0].kind == eb::BackendEvent::Kind::kPartial);
  CHECK(events[0].text == "第1帧(320)");
  CHECK(events[1].text == "第2帧(320)");
  CHECK(events[2].text == "第3帧(320)");
  CHECK(events[3].kind == eb::BackendEvent::Kind::kFinal);
  CHECK(events[3].text == "第1帧(320) 第2帧(320) 第3帧(320)");
  CHECK(events[3].pcm.empty());
  std::cout << "  [ok] 确定性 partial/final：3 帧 → 第1/2/3帧(320) + 空格连接 final" << std::endl;
}

// partial 文本携带实际帧长：不同采样数帧可区分。
void test_partial_reports_frame_length() {
  ef::FakeAsrBackend asr;
  auto events = collect(asr);
  asr.feed_audio({std::int16_t(1), std::int16_t(2), std::int16_t(3)}, true);
  CHECK(events.size() == 2);
  CHECK(events[0].text == "第1帧(3)");
  CHECK(events[1].kind == eb::BackendEvent::Kind::kFinal);
  CHECK(events[1].text == "第1帧(3)");
  std::cout << "  [ok] partial 携带帧长：第1帧(3)" << std::endl;
}

// 取消：cancel 后继续 feed 不再产出任何事件。
void test_cancel_stops_events() {
  ef::FakeAsrBackend asr;
  auto events = collect(asr);
  asr.feed_audio(ef::FakeAudioSource::make_frame(0), false);
  asr.cancel();
  asr.feed_audio(ef::FakeAudioSource::make_frame(1), true);

  CHECK(events.size() == 1);  // 只有取消前的一条 partial
  CHECK(events[0].kind == eb::BackendEvent::Kind::kPartial);
  std::cout << "  [ok] 取消：cancel 后 feed_audio 不再产出事件" << std::endl;
}

// 会话重置：set_event_callback 清空帧计数、累计文本与取消状态，
// 新会话从第 1 帧重新计数。
void test_session_reset() {
  ef::FakeAsrBackend asr;
  {
    auto events = collect(asr);
    asr.feed_audio(ef::FakeAudioSource::make_frame(0), false);
    asr.feed_audio(ef::FakeAudioSource::make_frame(1), true);
    CHECK(events.size() == 3);
    CHECK(events.back().text == "第1帧(320) 第2帧(320)");
  }
  {
    auto events = collect(asr);  // 新会话：计数与累计文本重置
    asr.feed_audio({std::int16_t(7)}, true);
    CHECK(events.size() == 2);
    CHECK(events[0].text == "第1帧(1)");
    CHECK(events.back().text == "第1帧(1)");
  }
  std::cout << "  [ok] 会话重置：新会话从第 1 帧重新计数" << std::endl;
}

// 会话重置同时清除取消状态：被取消的会话结束后，新会话仍可正常产出。
void test_session_reset_clears_cancel() {
  ef::FakeAsrBackend asr;
  {
    auto events = collect(asr);
    asr.feed_audio(ef::FakeAudioSource::make_frame(0), false);
    asr.cancel();
    CHECK(events.size() == 1);
  }
  {
    auto events = collect(asr);
    asr.feed_audio(ef::FakeAudioSource::make_frame(0), true);
    CHECK(events.size() == 2);  // 新会话不再受上次取消影响
    CHECK(events.back().kind == eb::BackendEvent::Kind::kFinal);
  }
  std::cout << "  [ok] 会话重置清除取消状态：新会话正常产出" << std::endl;
}

// FakeAudioSource 确定性：相同参数产出逐位相同的帧，公式可精确断言。
void test_audio_source_determinism() {
  const auto a = ef::FakeAudioSource::make_frame(0);
  const auto b = ef::FakeAudioSource::make_frame(0);
  CHECK(a == b);
  CHECK(a.size() == static_cast<std::size_t>(eb::kFrameSamples));

  // 公式 ((seq*977 + 下标*197) % 2048) - 1024：
  CHECK(a[0] == -1024);                              // seq=0, s=0
  CHECK(ef::FakeAudioSource::make_frame(1)[0] == -47);  // 977-1024
  CHECK(a[1] == -827);                               // 197-1024
  CHECK(ef::FakeAudioSource::make_frame(0, 3).size() == 3);
  std::cout << "  [ok] FakeAudioSource 确定性：同参同帧、公式可断言" << std::endl;
}

}  // namespace

int main() {
  std::cout << "fake_asr_test:" << std::endl;
  test_deterministic_partial_final();
  test_partial_reports_frame_length();
  test_cancel_stops_events();
  test_session_reset();
  test_session_reset_clears_cancel();
  test_audio_source_determinism();

  if (g_failures == 0) {
    std::cout << "fake_asr_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "fake_asr_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
