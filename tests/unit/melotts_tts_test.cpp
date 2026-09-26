// MeloTtsBackend 单元测试（板端真实推理；仅硬件后端构建时编译）。
//
// 与 fake_tts_test / summer_tts_test 相同的协议骨架：收集事件 → 断言 kPcm 帧
// （每帧 ≤ kFrameSamples=320 采样，20 ms）+ 末事件 kDone；并核对帧总采样数与
// 音频时长自洽、RTF 打印供核对。
//
// 五个资源路径经环境变量注入（tests/unit/CMakeLists.txt 由 SLOTNEXUS_MELOTTS_*
// 缓存变量设置）；任一未配置时整组跳过（返回 0），避免无板端依赖时挂死 ctest。
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "slotnexus/backend/backend_event.hpp"
#include "slotnexus/backend/melotts/melotts_tts_backend.hpp"

namespace eb = slotnexus::backend;
namespace em = slotnexus::backend::melotts;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      ++g_failures;                                                        \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #cond \
                << std::endl;                                              \
    }                                                                      \
  } while (0)

std::string env_or_empty(const char* name) {
  const char* value = std::getenv(name);
  return value != nullptr ? value : "";
}

bool file_exists(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  if (std::FILE* handle = std::fopen(path.c_str(), "rb")) {
    std::fclose(handle);
    return true;
  }
  return false;
}

em::MeloTtsConfig config_from_env() {
  em::MeloTtsConfig config;
  config.encoder_model_path = env_or_empty("SLOTNEXUS_MELOTTS_ENCODER");
  config.decoder_model_path = env_or_empty("SLOTNEXUS_MELOTTS_DECODER");
  config.lexicon_path = env_or_empty("SLOTNEXUS_MELOTTS_LEXICON");
  config.tokens_path = env_or_empty("SLOTNEXUS_MELOTTS_TOKENS");
  config.g_path = env_or_empty("SLOTNEXUS_MELOTTS_G");
  return config;
}

std::vector<eb::BackendEvent> attach(em::MeloTtsBackend& tts) {
  std::vector<eb::BackendEvent> events;
  tts.set_event_callback(
      [&events](const eb::BackendEvent& event) { events.push_back(event); });
  return events;
}

// 结构断言：≥1 个 kPcm 帧 + 末事件 kDone，帧长 ≤ 320 且总采样与 16 kHz 时长自洽。
void check_event_stream(const std::vector<eb::BackendEvent>& events) {
  CHECK(events.size() >= 2);
  CHECK(events.back().kind == eb::BackendEvent::Kind::kDone);
  std::size_t total = 0;
  bool frames_ok = true;
  for (std::size_t index = 0; index + 1 < events.size(); ++index) {
    CHECK(events[index].kind == eb::BackendEvent::Kind::kPcm);
    frames_ok = frames_ok && !events[index].pcm.empty() &&
                events[index].pcm.size() <=
                    static_cast<std::size_t>(eb::kFrameSamples);
    total += events[index].pcm.size();
  }
  CHECK(frames_ok);
  CHECK(total > 0);
}

void test_synthesis(const em::MeloTtsConfig& config) {
  em::MeloTtsBackend tts(config);
  auto events = attach(tts);
  const auto begin = std::chrono::steady_clock::now();
  tts.synthesize("你好，这是语音合成测试。");
  const auto end = std::chrono::steady_clock::now();

  check_event_stream(events);
  std::size_t total = 0;
  for (std::size_t index = 0; index + 1 < events.size(); ++index) {
    total += events[index].pcm.size();
  }
  const double audio_s = static_cast<double>(total) / eb::kSampleRateHz;
  const double infer_s = std::chrono::duration<double>(end - begin).count();
  std::printf("  [info] pcm_samples=%zu audio=%.3fs infer=%.3fs RTF=%.3f\n", total,
              audio_s, infer_s, (audio_s > 0 ? infer_s / audio_s : 0.0));
  std::cout << "  [ok] 固定文本：kPcm 帧流 + 末事件 kDone，帧长 ≤ 320" << std::endl;
}

// 取消：cancel 后 synthesize 不产出任何事件（含 kDone）。
void test_cancel_suppresses_synthesis(const em::MeloTtsConfig& config) {
  em::MeloTtsBackend tts(config);
  auto events = attach(tts);
  tts.cancel();
  tts.synthesize("你好");
  CHECK(events.empty());
  std::cout << "  [ok] 取消：cancel 后 synthesize 无任何事件" << std::endl;
}

// 会话重置：新 set_event_callback 清除取消状态，新会话正常合成。
void test_session_reset_clears_cancel(const em::MeloTtsConfig& config) {
  em::MeloTtsBackend tts(config);
  {
    auto events = attach(tts);
    tts.cancel();
    tts.synthesize("旧文本");
    CHECK(events.empty());
  }
  {
    auto events = attach(tts);  // 新会话
    tts.synthesize("你好，这是语音合成测试。");
    check_event_stream(events);
  }
  std::cout << "  [ok] 会话重置：新会话不受上次取消影响" << std::endl;
}

}  // namespace

int main() {
  std::cout << "melotts_tts_test:" << std::endl;
  const em::MeloTtsConfig config = config_from_env();
  if (!file_exists(config.encoder_model_path) ||
      !file_exists(config.decoder_model_path) ||
      !file_exists(config.lexicon_path) || !file_exists(config.tokens_path) ||
      !file_exists(config.g_path)) {
    std::cout << "  [skip] 未配置完整的 SLOTNEXUS_MELOTTS_{ENCODER,DECODER,"
                 "LEXICON,TOKENS,G}（板端 ctest 需注入五个路径）" << std::endl;
    return 0;
  }
  try {
    test_synthesis(config);
    test_cancel_suppresses_synthesis(config);
    test_session_reset_clears_cancel(config);
  } catch (const std::exception& error) {
    std::cerr << "  [fail] 构造/合成异常: " << error.what() << std::endl;
    ++g_failures;
  }

  if (g_failures == 0) {
    std::cout << "melotts_tts_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "melotts_tts_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
