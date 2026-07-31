// RkllmBackend 单元测试（板端真实大模型；仅硬件后端构建时编译）。
//
// 与 fake_llm_test 相同的协议骨架：收集事件 → 断言 kToken 流 + 末事件
// kDone（kDone 携带完整输出文本）；固定 prompt 与门禁 smoke 同款
// （"你好，请用一句话介绍你自己。"，见 artifacts/upstream-baseline/
// rkllm_smoke.cpp），top_k=1 贪心采样输出确定性，文本与指标（TTFT / tok/s）
// 记录进证据文档对照。模型路径经环境变量 VOXORCHESTRA_RKLLM_MODEL 注入
// （tests/unit/CMakeLists.txt 由 VOXORCHESTRA_RKLLM_MODEL 缓存变量设置）；
// 未配置时整组跳过（返回 0）。R1 推理模型输出文本不作逐字断言（受
// max_new_tokens 与上下文影响），只断言结构 + 非空。
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "voxorchestra/backend/backend_event.hpp"
#include "voxorchestra/backend/rkllm/rkllm_llm_backend.hpp"

namespace eb = voxorchestra::backend;
namespace er = voxorchestra::backend::rkllm;

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

std::string model_from_env() {
  const char* p = std::getenv("VOXORCHESTRA_RKLLM_MODEL");
  return p != nullptr ? p : "";
}

// 门禁 smoke 同款固定 prompt（upstream-baseline.md 门禁记录）。
const char* kSmokePrompt = "你好，请用一句话介绍你自己。";

// 收集一次生成会话的全部事件；记录首个事件与结束时的时间戳用于指标。
struct Session {
  std::vector<eb::BackendEvent> events;
  std::chrono::steady_clock::time_point first;  // 首个事件时刻
  std::chrono::steady_clock::time_point last;   // 末事件时刻
  std::chrono::steady_clock::time_point start;  // generate 前
};

// 结构断言：kToken 流（全部非空）+ 末事件 kDone，kDone.text = token 拼接。
// 返回完整输出文本，供证据记录。
std::string check_structure(Session& s, const char* what) {
  CHECK(s.events.size() >= 2);
  CHECK(s.events.back().kind == eb::BackendEvent::Kind::kDone);
  std::string concat;
  for (std::size_t i = 0; i + 1 < s.events.size(); ++i) {
    CHECK(s.events[i].kind == eb::BackendEvent::Kind::kToken);
    CHECK(!s.events[i].text.empty());
    concat += s.events[i].text;
  }
  CHECK(s.events.back().text == concat);
  CHECK(!s.events.back().text.empty());
  const double total_s =
      std::chrono::duration<double>(s.last - s.start).count();
  const double ttft_ms = std::chrono::duration<double, std::milli>(
                             s.first - s.start)
                             .count();
  const int tokens = static_cast<int>(s.events.size() - 1);
  const double decode_s = std::chrono::duration<double>(s.last - s.first).count();
  std::printf(
      "  [info] %s: tokens=%d TTFT_ms=%.1f total_s=%.2f tok_per_s=%.2f\n",
      what, tokens, ttft_ms, total_s, (decode_s > 0 ? tokens / decode_s : 0.0));
  std::cout << "  [info] " << what << " 输出文本: [" << s.events.back().text
            << "]" << std::endl;
  return s.events.back().text;
}

// 固定 prompt 与 smoke 对照：kToken 流 + 末事件 kDone（完整输出），
// 结构断言 + 指标打印（TTFT / tok/s，板端与门禁基线 288 ms / 7.79 tok/s
// 对照，证据入 artifacts/llm-integration/）。
void test_fixed_prompt_matches_smoke(const std::string& model) {
  er::RkllmBackend llm(model);
  Session s;
  s.start = std::chrono::steady_clock::now();
  llm.set_event_callback([&s](const eb::BackendEvent& e) {
    if (s.events.empty()) {
      s.first = std::chrono::steady_clock::now();
    }
    s.events.push_back(e);
    s.last = std::chrono::steady_clock::now();
  });
  llm.generate(kSmokePrompt);
  check_structure(s, "固定 prompt");
  std::cout << "  [ok] 固定 prompt：kToken 流 + 末事件 kDone，输出非空"
            << std::endl;
}

// 取消：cancel 后 generate 不产出任何事件（含 kDone）。
void test_cancel_suppresses_generation(const std::string& model) {
  er::RkllmBackend llm(model);
  std::vector<eb::BackendEvent> events;
  llm.set_event_callback([&events](const eb::BackendEvent& e) {
    events.push_back(e);
  });
  llm.cancel();
  llm.generate(kSmokePrompt);
  CHECK(events.empty());
  std::cout << "  [ok] 取消：cancel 后 generate 无任何事件" << std::endl;
}

// 生成中取消：已投递的旧 token 之后不得再有新事件（过滤点在泵队列），
// 且不得补发 kDone。
void test_cancel_filters_inflight(const std::string& model) {
  er::RkllmBackend llm(model);
  std::vector<eb::BackendEvent> events;
  std::mutex mu;
  llm.set_event_callback([&events, &mu](const eb::BackendEvent& e) {
    std::lock_guard<std::mutex> lk(mu);
    events.push_back(e);
  });
  std::atomic<bool> done{false};
  std::thread worker([&llm, &done] {
    llm.generate(kSmokePrompt);
    done.store(true);
  });
  // 等首个 token 落地后再取消（TTFT 板端 ~0.3 s，60 s 上限防卡死）。
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (std::chrono::steady_clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> lk(mu);
      if (!events.empty()) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(!events.empty());
  llm.cancel();
  // 留出窗口让已入队事件处理完，再确认停止增长。
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  const std::size_t frozen = [&] {
    std::lock_guard<std::mutex> lk(mu);
    return events.size();
  }();
  worker.join();
  CHECK(done.load());
  CHECK(events.size() == frozen);
  bool has_done = false;
  for (const auto& e : events) {
    has_done = has_done || e.kind == eb::BackendEvent::Kind::kDone;
  }
  CHECK(!has_done);
  std::cout << "  [ok] 生成中取消：cancel 后无新事件、无 kDone（旧 token 过滤）"
            << std::endl;
}

// 会话重置：新 set_event_callback 清除取消状态，新会话正常生成。
void test_session_reset_clears_cancel(const std::string& model) {
  er::RkllmBackend llm(model);
  {
    std::vector<eb::BackendEvent> events;
    llm.set_event_callback([&events](const eb::BackendEvent& e) {
      events.push_back(e);
    });
    llm.cancel();
    llm.generate(kSmokePrompt);
    CHECK(events.empty());
  }
  {
    Session s;
    s.start = std::chrono::steady_clock::now();
    llm.set_event_callback([&s](const eb::BackendEvent& e) {
      if (s.events.empty()) {
        s.first = std::chrono::steady_clock::now();
      }
      s.events.push_back(e);
      s.last = std::chrono::steady_clock::now();
    });
    llm.generate(kSmokePrompt);
    check_structure(s, "会话重置");
  }
  std::cout << "  [ok] 会话重置：新会话不受上次取消影响" << std::endl;
}

}  // namespace

int main() {
  std::cout << "rkllm_llm_test:" << std::endl;
  const std::string model = model_from_env();
  if (model.empty()) {
    std::cout << "  [skip] 未配置 VOXORCHESTRA_RKLLM_MODEL（板端 ctest 需 "
                 "-DVOXORCHESTRA_RKLLM_MODEL=<模型路径>）" << std::endl;
    return 0;
  }
  if (std::FILE* f = std::fopen(model.c_str(), "rb")) {
    std::fclose(f);
  } else {
    std::cerr << "  [fail] 模型文件不存在: " << model << std::endl;
    return 1;
  }
  try {
    test_fixed_prompt_matches_smoke(model);
    test_cancel_suppresses_generation(model);
    test_cancel_filters_inflight(model);
    test_session_reset_clears_cancel(model);
  } catch (const std::exception& e) {
    std::cerr << "  [fail] 构造/生成异常: " << e.what() << std::endl;
    ++g_failures;
  }

  if (g_failures == 0) {
    std::cout << "rkllm_llm_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "rkllm_llm_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
