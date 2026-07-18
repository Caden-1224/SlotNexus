// FakeLlmBackend 单元测试：确定性 token 序列、最终文本、取消语义与会话重置。
#include "voxorchestra/backend/backend_event.hpp"
#include "voxorchestra/backend/fake/fake_llm_backend.hpp"

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

// 收集一次生成会话的全部事件。
std::vector<eb::BackendEvent> collect(ef::FakeLlmBackend& llm) {
  std::vector<eb::BackendEvent> events;
  llm.set_event_callback([&events](const eb::BackendEvent& e) { events.push_back(e); });
  return events;
}

// 确定性：多词 prompt 逐 token 产出，kDone 携带完整输出。
void test_token_sequence() {
  ef::FakeLlmBackend llm;
  auto events = collect(llm);
  llm.generate("你好 世界 VoxOrchestra");

  CHECK(events.size() == 4);  // 3 token + 1 done
  CHECK(events[0].kind == eb::BackendEvent::Kind::kToken);
  CHECK(events[0].text == "你好");
  CHECK(events[1].text == "世界");
  CHECK(events[2].text == "VoxOrchestra");
  CHECK(events[3].kind == eb::BackendEvent::Kind::kDone);
  CHECK(events[3].text == "你好 世界 VoxOrchestra");
  CHECK(events[3].pcm.empty());
  std::cout << "  [ok] token 序列：逐词 kToken + kDone 携带完整输出" << std::endl;
}

// 无空白 prompt：单个 token；空 prompt：仅 kDone（空输出）。
void test_single_word_and_empty() {
  ef::FakeLlmBackend llm;
  {
    auto events = collect(llm);
    llm.generate("hello");
    CHECK(events.size() == 2);
    CHECK(events[0].text == "hello");
    CHECK(events[1].kind == eb::BackendEvent::Kind::kDone);
    CHECK(events[1].text == "hello");
  }
  {
    auto events = collect(llm);
    llm.generate("");
    CHECK(events.size() == 1);  // 空 prompt 无 token，仍产出 kDone
    CHECK(events[0].kind == eb::BackendEvent::Kind::kDone);
    CHECK(events[0].text.empty());
  }
  std::cout << "  [ok] 单词与空 prompt：单词单 token、空串直接 kDone" << std::endl;
}

// 连续空白折叠：输出文本为单空格归一化，token 序列不受影响。
void test_whitespace_normalization() {
  ef::FakeLlmBackend llm;
  auto events = collect(llm);
  llm.generate("a   b\tc");

  CHECK(events.size() == 4);
  CHECK(events[0].text == "a");
  CHECK(events[1].text == "b");
  CHECK(events[2].text == "c");
  CHECK(events[3].text == "a b c");
  std::cout << "  [ok] 空白归一化：连续空白/制表符折叠为单空格" << std::endl;
}

// 取消：cancel 后 generate 不产出任何事件（含 kDone）。
void test_cancel_suppresses_generation() {
  ef::FakeLlmBackend llm;
  auto events = collect(llm);
  llm.cancel();
  llm.generate("你好 世界");

  CHECK(events.empty());
  std::cout << "  [ok] 取消：cancel 后 generate 无任何事件" << std::endl;
}

// 会话重置：新 set_event_callback 清除取消状态，新会话正常生成。
void test_session_reset_clears_cancel() {
  ef::FakeLlmBackend llm;
  {
    auto events = collect(llm);
    llm.cancel();
    llm.generate("旧会话");
    CHECK(events.empty());
  }
  {
    auto events = collect(llm);  // 新会话
    llm.generate("新会话");
    CHECK(events.size() == 2);
    CHECK(events.back().kind == eb::BackendEvent::Kind::kDone);
    CHECK(events.back().text == "新会话");
  }
  std::cout << "  [ok] 会话重置：新会话不受上次取消影响" << std::endl;
}

// 确定性：相同 prompt 两次生成的事件序列逐字段一致。
void test_determinism() {
  ef::FakeLlmBackend llm;
  std::vector<eb::BackendEvent> a;
  std::vector<eb::BackendEvent> b;
  llm.set_event_callback([&a](const eb::BackendEvent& e) { a.push_back(e); });
  llm.generate("重复 生成 测试");
  llm.set_event_callback([&b](const eb::BackendEvent& e) { b.push_back(e); });
  llm.generate("重复 生成 测试");

  CHECK(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    CHECK(a[i].kind == b[i].kind);
    CHECK(a[i].text == b[i].text);
  }
  std::cout << "  [ok] 确定性：同 prompt 两次生成序列完全一致" << std::endl;
}

}  // namespace

int main() {
  std::cout << "fake_llm_test:" << std::endl;
  test_token_sequence();
  test_single_word_and_empty();
  test_whitespace_normalization();
  test_cancel_suppresses_generation();
  test_session_reset_clears_cancel();
  test_determinism();

  if (g_failures == 0) {
    std::cout << "fake_llm_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "fake_llm_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
