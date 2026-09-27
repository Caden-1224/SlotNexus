// RkllmBackend 单元测试（板端真实大模型；仅硬件后端构建时编译）。
// Author: Caden
//
// 与 fake_llm_test 相同的协议骨架：收集事件 → 断言 kToken 流 + 末事件
// kDone（kDone 携带完整输出文本）；固定 prompt 与门禁 smoke 同款
// （"你好，请用一句话介绍你自己。"，见 modules/voice/tools/upstream-probes/
// rkllm_smoke.cpp），top_k=1 贪心采样输出确定性，文本与指标（TTFT / tok/s）
// 记录进证据文档对照。模型路径经环境变量 SLOTNEXUS_RKLLM_MODEL 注入
// （tests/unit/CMakeLists.txt 由 SLOTNEXUS_RKLLM_MODEL 缓存变量设置）；
// 未配置时整组跳过（返回 0）。
// 采样参数与思考模式同样可用环境变量覆盖（见 options_from_env），因此同一
// 块板可以在不改仓库的情况下对比不同模型：例如把 SLOTNEXUS_RKLLM_MODEL
// 指向 Qwen3.5-0.8B，配合 SLOTNEXUS_RKLLM_ENABLE_THINKING=1 观察思考段
// 是否被 reasoning_end_tag 过滤干净。
// 输出文本不作逐字断言（受模型与 max_new_tokens 影响），只断言结构 + 非空；
// 启用思考段过滤时额外断言事件流中不残留过滤标记。
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

#include "slotnexus/backend/backend_event.hpp"
#include "slotnexus/backend/rkllm/rkllm_llm_backend.hpp"
#include "slotnexus/backend/rkllm/rkllm_options.hpp"

namespace eb = slotnexus::backend;
namespace er = slotnexus::backend::rkllm;

namespace {

int g_failures = 0;
std::string g_reasoning_tag;  // 仅用于“事件流不残留过滤标记”的断言

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #cond   \
                << std::endl;                                                \
    }                                                                        \
  } while (0)

std::string env_or(const char* name, const std::string& fallback) {
  const char* p = std::getenv(name);
  return (p != nullptr && *p != '\0') ? std::string(p) : fallback;
}

std::string model_from_env() {
  return env_or("SLOTNEXUS_RKLLM_MODEL", "");
}

// 选项来源：环境变量（默认值与 config/taishanpi3m/session.json 一致）。
// 取值 "-" 或 "off" 表示空串（用于关闭思考段过滤）。
er::RkllmOptions options_from_env() {
  er::RkllmOptions o;
  o.model_path = model_from_env();
  o.max_new_tokens =
      std::atoi(env_or("SLOTNEXUS_RKLLM_MAX_NEW_TOKENS", "100").c_str());
  o.max_context_len =
      std::atoi(env_or("SLOTNEXUS_RKLLM_MAX_CONTEXT_LEN", "256").c_str());
  o.top_k = std::atoi(env_or("SLOTNEXUS_RKLLM_TOP_K", "1").c_str());
  o.top_p = std::stof(env_or("SLOTNEXUS_RKLLM_TOP_P", "0.95"));
  o.temperature = std::stof(env_or("SLOTNEXUS_RKLLM_TEMPERATURE", "0.8"));
  o.repeat_penalty =
      std::stof(env_or("SLOTNEXUS_RKLLM_REPEAT_PENALTY", "1.1"));
  o.enable_thinking = env_or("SLOTNEXUS_RKLLM_ENABLE_THINKING", "0") == "1";
  const std::string tag =
      env_or("SLOTNEXUS_RKLLM_REASONING_END_TAG", "</think>");
  o.reasoning_end_tag = (tag == "-" || tag == "off") ? std::string() : tag;
  return o;
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
  // 启用思考段过滤时：事件流（含 kDone）中不得残留过滤标记。
  if (!g_reasoning_tag.empty()) {
    for (const auto& e : s.events) {
      CHECK(e.text.find(g_reasoning_tag) == std::string::npos);
    }
  }
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
// 结构断言 + 指标打印（TTFT / tok/s，板端与门禁基线 288 ms / 7.79 tok/s 对照）。
void test_fixed_prompt_matches_smoke(const er::RkllmOptions& options) {
  er::RkllmBackend llm(options);
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
void test_cancel_suppresses_generation(const er::RkllmOptions& options) {
  er::RkllmBackend llm(options);
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
void test_cancel_filters_inflight(const er::RkllmOptions& options) {
  er::RkllmBackend llm(options);
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
void test_session_reset_clears_cancel(const er::RkllmOptions& options) {
  er::RkllmBackend llm(options);
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
  const er::RkllmOptions options = options_from_env();
  if (options.model_path.empty()) {
    std::cout << "  [skip] 未配置 SLOTNEXUS_RKLLM_MODEL（板端 ctest 需 "
                 "-DSLOTNEXUS_RKLLM_MODEL=<模型路径>）" << std::endl;
    return 0;
  }
  if (std::FILE* f = std::fopen(options.model_path.c_str(), "rb")) {
    std::fclose(f);
  } else {
    std::cerr << "  [fail] 模型文件不存在: " << options.model_path << std::endl;
    return 1;
  }
  const std::string reason = er::validate(options);
  if (!reason.empty()) {
    std::cerr << "  [fail] 选项非法: " << reason << std::endl;
    return 1;
  }
  g_reasoning_tag = options.reasoning_end_tag;
  std::cout << "  [info] 模型 " << options.model_path << "，max_new_tokens "
            << options.max_new_tokens << " / max_context_len "
            << options.max_context_len << "，top_k " << options.top_k
            << " / top_p " << options.top_p << " / temperature "
            << options.temperature << "，enable_thinking "
            << (options.enable_thinking ? 1 : 0) << "，reasoning_end_tag "
            << (options.reasoning_end_tag.empty()
                    ? std::string("<off>")
                    : options.reasoning_end_tag)
            << std::endl;
  try {
    test_fixed_prompt_matches_smoke(options);
    test_cancel_suppresses_generation(options);
    test_cancel_filters_inflight(options);
    test_session_reset_clears_cancel(options);
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
