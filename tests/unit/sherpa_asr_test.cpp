// SherpaAsrBackend 单元测试（板端真实 ASR；仅硬件后端构建时编译）。
//
// 与 fake_asr_test 相同的协议骨架：收集事件 → 断言 kPartial 渐进 + 末事件
// kFinal；并核对识别结果与门禁基线一致（test_wavs/0.wav 固定音频、4 线程
// 最终文本，见 artifacts/upstream-baseline/upstream-baseline.md 第二段）。
// 模型目录经环境变量 SLOTNEXUS_ASR_MODEL 注入（tests/unit/CMakeLists.txt
// 由 SLOTNEXUS_ASR_MODEL 缓存变量设置）；未配置时整组跳过（返回 0）。
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "slotnexus/backend/backend_event.hpp"
#include "slotnexus/backend/sherpa_onnx/sherpa_asr_backend.hpp"
#include "slotnexus/common/wav_reader.hpp"

namespace eb = slotnexus::backend;
namespace es = slotnexus::backend::sherpa_onnx;
using slotnexus::common::WavReader;

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

std::string model_dir_from_env() {
  const char* p = std::getenv("SLOTNEXUS_ASR_MODEL");
  return p != nullptr ? p : "";
}

// 固定 WAV（test_wavs/0.wav）按 20 ms 帧（320 采样）喂入，收集全部事件。
std::vector<eb::BackendEvent> feed_wav(es::SherpaAsrBackend& asr,
                                       const std::string& wav_path) {
  const auto r = WavReader::read(wav_path);
  CHECK(r.ok);
  if (!r.ok) {
    return {};
  }
  std::vector<eb::BackendEvent> events;
  asr.set_event_callback([&events](const eb::BackendEvent& e) {
    events.push_back(e);
  });
  const auto& samples = r.info.samples;
  std::size_t offset = 0;
  while (offset < samples.size()) {
    const std::size_t n = std::min<std::size_t>(
        static_cast<std::size_t>(eb::kFrameSamples), samples.size() - offset);
    const std::vector<int16_t> frame(samples.begin() + offset,
                                     samples.begin() + offset + n);
    offset += n;
    asr.feed_audio(frame, offset >= samples.size());
  }
  return events;
}

// 固定 WAV（门禁基线同款）：kPartial 渐进（文本随块更新），末事件 kFinal，
// 最终文本与门禁基线一致；打印 RTF 供核对。
void test_fixed_wav_matches_baseline(const std::string& model_dir) {
  es::SherpaAsrBackend asr(model_dir, /*num_threads=*/4);
  const std::string wav_path = model_dir + "/test_wavs/0.wav";
  const auto t0 = std::chrono::steady_clock::now();
  auto events = feed_wav(asr, wav_path);
  const auto t1 = std::chrono::steady_clock::now();

  CHECK(events.size() >= 2);
  CHECK(events.back().kind == eb::BackendEvent::Kind::kFinal);
  bool partials_ok = true;
  for (std::size_t i = 0; i + 1 < events.size(); ++i) {
    partials_ok = partials_ok &&
                  events[i].kind == eb::BackendEvent::Kind::kPartial &&
                  !events[i].text.empty();
  }
  CHECK(partials_ok);
  // 门禁基线最终文本（4 threads）。该字符串与 **ASR 运行时版本链** 绑定：
  //   - 上一代链（板端源码编译的 sherpa-onnx + ONNX Runtime 1.17.1）：
  //     "昨天是 MONDAY TODAYS TOMORROW是星"
  //   - 当前链（sherpa-onnx v1.13.8 官方 linux-aarch64 shared-cpu 预编译件，
  //     自带 ONNX Runtime 1.28.2，tokens.txt 取同版本官方模型包）：
  //     "昨天是 MONDAY TODAY IS THE AFTER TOMORROW是星"
  // 两者用的是同一份 int8 模型（encoder/decoder/joiner 的 SHA256 与
  // upstream-baseline.md 逐项一致），差异来自词典与运行时版本，不是模型替换。
  // 换模型、tokens.txt 或 ASR 运行时后必须重新核验并同步更新本行。
  // 证据见 artifacts/environment-preflight/board-runtime-restore.md。
  CHECK(events.back().text == "昨天是 MONDAY TODAY IS THE AFTER TOMORROW是星");

  const auto r = WavReader::read(wav_path);
  const double audio_s =
      static_cast<double>(r.ok ? r.info.samples.size() : 0) / eb::kSampleRateHz;
  const double infer_s = std::chrono::duration<double>(t1 - t0).count();
  std::printf("  [info] final=\"%s\" audio=%.2fs infer=%.2fs RTF=%.3f\n",
              events.back().text.c_str(), audio_s, infer_s,
              (audio_s > 0 ? infer_s / audio_s : 0.0));
  std::cout << "  [ok] 固定 WAV：kPartial 渐进 + 末事件 kFinal，文本与门禁基线一致"
            << std::endl;
}

// 取消：cancel 后 feed_audio 不产出任何事件（含 kFinal）。
void test_cancel_suppresses_feed(const std::string& model_dir) {
  es::SherpaAsrBackend asr(model_dir, 4);
  std::vector<eb::BackendEvent> events;
  asr.set_event_callback([&events](const eb::BackendEvent& e) {
    events.push_back(e);
  });
  asr.cancel();
  asr.feed_audio({1, 2, 3}, /*is_last=*/true);
  CHECK(events.empty());
  std::cout << "  [ok] 取消：cancel 后 feed_audio 无任何事件" << std::endl;
}

// 会话重置：新 set_event_callback 清除取消状态，新会话正常识别。
void test_session_reset_clears_cancel(const std::string& model_dir) {
  es::SherpaAsrBackend asr(model_dir, 4);
  {
    std::vector<eb::BackendEvent> events;
    asr.set_event_callback([&events](const eb::BackendEvent& e) {
      events.push_back(e);
    });
    asr.cancel();
    asr.feed_audio({1, 2, 3}, /*is_last=*/true);
    CHECK(events.empty());
  }
  {
    auto events = feed_wav(asr, model_dir + "/test_wavs/0.wav");  // 新会话
    CHECK(events.size() >= 2);
    CHECK(events.back().kind == eb::BackendEvent::Kind::kFinal);
  }
  std::cout << "  [ok] 会话重置：新会话不受上次取消影响" << std::endl;
}

}  // namespace

int main() {
  std::cout << "sherpa_asr_test:" << std::endl;
  const std::string model_dir = model_dir_from_env();
  if (model_dir.empty()) {
    std::cout << "  [skip] 未配置 SLOTNEXUS_ASR_MODEL（板端 ctest 需 "
                 "-DSLOTNEXUS_ASR_MODEL=<模型目录>）" << std::endl;
    return 0;
  }
  const std::string wav_path = model_dir + "/test_wavs/0.wav";
  if (std::FILE* f = std::fopen(wav_path.c_str(), "rb")) {
    std::fclose(f);
  } else {
    std::cerr << "  [fail] 测试 WAV 不存在: " << wav_path << std::endl;
    return 1;
  }
  try {
    test_fixed_wav_matches_baseline(model_dir);
    test_cancel_suppresses_feed(model_dir);
    test_session_reset_clears_cancel(model_dir);
  } catch (const std::exception& e) {
    std::cerr << "  [fail] 构造/识别异常: " << e.what() << std::endl;
    ++g_failures;
  }

  if (g_failures == 0) {
    std::cout << "sherpa_asr_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "sherpa_asr_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
