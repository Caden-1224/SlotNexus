// SummerTtsBackend 单元测试（板端真实 vits；仅硬件后端构建时编译）。
//
// 与 fake_tts_test 相同的协议骨架：收集事件 → 断言 kPcm 帧 + kDone；
// 并核对合成结果与门禁基线一致（固定文本 dataLen=53248，16 kHz mono S16，
// 见 artifacts/upstream-baseline/upstream-baseline.md 第三段）。
// 模型路径经环境变量 VOXORCHESTRA_TTS_MODEL 注入（tests/unit/CMakeLists.txt
// 由 VOXORCHESTRA_TTS_MODEL 缓存变量设置）；未配置时整组跳过（返回 0）。
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "voxorchestra/backend/backend_event.hpp"
#include "voxorchestra/backend/summer_tts/summer_tts_backend.hpp"

namespace eb = voxorchestra::backend;
namespace es = voxorchestra::backend::summer_tts;

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

std::string model_path_from_env() {
  const char* p = std::getenv("VOXORCHESTRA_TTS_MODEL");
  return p != nullptr ? p : "";
}

// 收集一次合成会话的全部事件。
std::vector<eb::BackendEvent> collect(es::SummerTtsBackend& tts) {
  std::vector<eb::BackendEvent> events;
  tts.set_event_callback([&events](const eb::BackendEvent& e) { events.push_back(e); });
  return events;
}

// 固定文本（门禁基线同款）：kPcm 帧总采样 = dataLen = 53248，
// 每帧 ≤ 320 采样（余数在最后一帧），末事件 kDone；打印 RTF 供核对。
void test_synthesis_matches_baseline(const std::string& model_path) {
  es::SummerTtsBackend tts(model_path, 1.0f);
  auto events = collect(tts);
  const auto t0 = std::chrono::steady_clock::now();
  tts.synthesize("你好，这是语音合成测试。");
  const auto t1 = std::chrono::steady_clock::now();

  CHECK(events.size() >= 2);
  CHECK(events.back().kind == eb::BackendEvent::Kind::kDone);
  std::size_t total = 0;
  bool frames_ok = true;
  for (std::size_t i = 0; i + 1 < events.size(); ++i) {
    CHECK(events[i].kind == eb::BackendEvent::Kind::kPcm);
    frames_ok = frames_ok && !events[i].pcm.empty() &&
                events[i].pcm.size() <= static_cast<std::size_t>(eb::kFrameSamples);
    total += events[i].pcm.size();
  }
  CHECK(frames_ok);
  CHECK(total == 53248);  // 门禁基线 dataLen（同模型同文本同语速，换模型需更新）

  const double audio_s = static_cast<double>(total) / eb::kSampleRateHz;
  const double infer_s = std::chrono::duration<double>(t1 - t0).count();
  std::printf("  [info] dataLen=%zu audio=%.3fs infer=%.3fs RTF=%.3f\n", total,
              audio_s, infer_s, (audio_s > 0 ? infer_s / audio_s : 0.0));
  std::cout << "  [ok] 固定文本：kPcm 帧总采样 = dataLen，末事件 kDone" << std::endl;
}

// 取消：cancel 后 synthesize 不产出任何事件（含 kDone）。
void test_cancel_suppresses_synthesis(const std::string& model_path) {
  es::SummerTtsBackend tts(model_path, 1.0f);
  auto events = collect(tts);
  tts.cancel();
  tts.synthesize("你好");
  CHECK(events.empty());
  std::cout << "  [ok] 取消：cancel 后 synthesize 无任何事件" << std::endl;
}

// 会话重置：新 set_event_callback 清除取消状态，新会话正常合成。
void test_session_reset_clears_cancel(const std::string& model_path) {
  es::SummerTtsBackend tts(model_path, 1.0f);
  {
    auto events = collect(tts);
    tts.cancel();
    tts.synthesize("旧文本");
    CHECK(events.empty());
  }
  {
    auto events = collect(tts);  // 新会话
    tts.synthesize("你好，这是语音合成测试。");
    CHECK(events.size() >= 2);
    CHECK(events.back().kind == eb::BackendEvent::Kind::kDone);
  }
  std::cout << "  [ok] 会话重置：新会话不受上次取消影响" << std::endl;
}

}  // namespace

int main() {
  std::cout << "summer_tts_test:" << std::endl;
  const std::string model = model_path_from_env();
  if (model.empty()) {
    std::cout << "  [skip] 未配置 VOXORCHESTRA_TTS_MODEL（板端 ctest 需 "
                 "-DVOXORCHESTRA_TTS_MODEL=<模型路径>）" << std::endl;
    return 0;
  }
  if (std::FILE* f = std::fopen(model.c_str(), "rb")) {
    std::fclose(f);
  } else {
    std::cerr << "  [fail] 模型文件不存在: " << model << std::endl;
    return 1;
  }
  try {
    test_synthesis_matches_baseline(model);
    test_cancel_suppresses_synthesis(model);
    test_session_reset_clears_cancel(model);
  } catch (const std::exception& e) {
    std::cerr << "  [fail] 构造/合成异常: " << e.what() << std::endl;
    ++g_failures;
  }

  if (g_failures == 0) {
    std::cout << "summer_tts_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "summer_tts_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
