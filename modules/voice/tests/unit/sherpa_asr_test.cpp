// SherpaAsrBackend 单元测试（板端真实 ASR；仅硬件后端构建时编译）。
// Author: Caden
//
// 协议骨架：收集事件 → 断言 kPartial 渐进 + 末事件 kFinal；并核对识别结果
// 与门禁基线一致。覆盖两个固定 WAV：
//   - test_wavs/0.wav：模型自带长句中英混合门禁；
//   - demo_zh.wav：链路发布用固定中文 fixture（有可信参考文本），必须由
//     SLOTNEXUS_VOICE_FIXTURE_DIR 注入。
// 模型目录经 SLOTNEXUS_ASR_MODEL、模型精度经 SLOTNEXUS_ASR_PRECISION
//（fp32/int8，默认 fp32）注入；未配置模型目录时整组跳过（返回 0），未配置
// fixture 目录时直接失败，避免把“未验证”计为通过。
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
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

std::string env_or_default(const char* name, const std::string& fallback) {
  const char* p = std::getenv(name);
  return (p != nullptr && *p != '\0') ? std::string(p) : fallback;
}

// 固定 WAV 按 20 ms 帧（320 采样）喂入，收集全部事件。
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

bool file_exists(const std::string& path) {
  if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
    std::fclose(f);
    return true;
  }
  return false;
}

// 共用的固定 WAV 断言：kPartial 渐进、末事件 kFinal、最终文本精确匹配。
void check_wav_text(const std::string& label, const std::string& model_dir,
                    const std::string& precision, const std::string& wav_path,
                    const std::string& expected_text) {
  CHECK(file_exists(wav_path));
  if (!file_exists(wav_path)) {
    return;
  }
  es::SherpaAsrBackend asr(model_dir, /*num_threads=*/4, precision);
  const auto t0 = std::chrono::steady_clock::now();
  auto events = feed_wav(asr, wav_path);
  const auto t1 = std::chrono::steady_clock::now();

  CHECK(events.size() >= 2);
  if (events.size() < 2) {
    return;
  }
  CHECK(events.back().kind == eb::BackendEvent::Kind::kFinal);
  bool partials_ok = true;
  for (std::size_t i = 0; i + 1 < events.size(); ++i) {
    partials_ok = partials_ok &&
                  events[i].kind == eb::BackendEvent::Kind::kPartial &&
                  !events[i].text.empty();
  }
  CHECK(partials_ok);
  CHECK(events.back().text == expected_text);

  const auto r = WavReader::read(wav_path);
  const double audio_s =
      static_cast<double>(r.ok ? r.info.samples.size() : 0) / eb::kSampleRateHz;
  const double infer_s = std::chrono::duration<double>(t1 - t0).count();
  std::printf("  [info] %s final=\"%s\" expected=\"%s\" audio=%.2fs "
              "infer=%.2fs RTF=%.3f\n",
              label.c_str(), events.back().text.c_str(), expected_text.c_str(),
              audio_s, infer_s, (audio_s > 0 ? infer_s / audio_s : 0.0));
  if (events.back().text == expected_text) {
    std::cout << "  [ok] " << label
              << "：kPartial 渐进 + 末事件 kFinal，文本与参考一致" << std::endl;
  } else {
    std::cerr << "  [fail] " << label << "：ASR 文本与参考不一致" << std::endl;
  }
}

// 模型自带 0.wav 门禁：官方 fp32 与官方 int8 配方使用同一文本基线，
// 防止把旧版“三个 int8”配方或不完整模型路径误当成通过。
void test_model_test_wav(const std::string& model_dir,
                         const std::string& precision) {
  const std::string wav_path = model_dir + "/test_wavs/0.wav";
  // 官方 fp32 与官方 int8 配方（encoder int8 + decoder fp32 + joiner int8）
  // 在 0.wav 上给出同一文本；只有 demo_zh 短句会暴露 int8 的量化误差。
  check_wav_text("test_wavs/0.wav", model_dir, precision, wav_path,
                 "昨天是 MONDAY TODAY IS THEY AFTER TOMORROW是星期三");
}

// demo_zh.wav 是链路发布用固定中文 fixture：板端 SummerTTS 以
// “你好，这是语音合成测试。”合成，参考文本可追溯到上一代 TTS 基线。
// 该用例必须覆盖真实引擎 + WavReader + 20 ms 分块喂入，不允许只测 0.wav。
void test_demo_fixture(const std::string& model_dir,
                       const std::string& precision,
                       const std::string& fixture_dir) {
  const std::string wav_path = fixture_dir + "/demo_zh.wav";
  check_wav_text("demo_zh.wav", model_dir, precision, wav_path,
                 "你好这是语音合成测试");
}

// 取消：cancel 后 feed_audio 不产出任何事件（含 kFinal）。
void test_cancel_suppresses_feed(const std::string& model_dir,
                                 const std::string& precision) {
  es::SherpaAsrBackend asr(model_dir, 4, precision);
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
void test_session_reset_clears_cancel(const std::string& model_dir,
                                      const std::string& precision) {
  es::SherpaAsrBackend asr(model_dir, 4, precision);
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
    if (events.size() >= 2) {
      CHECK(events.back().kind == eb::BackendEvent::Kind::kFinal);
    }
  }
  std::cout << "  [ok] 会话重置：新会话不受上次取消影响" << std::endl;
}

}  // namespace

int main() {
  std::cout << "sherpa_asr_test:" << std::endl;
  const std::string model_dir = env_or_default("SLOTNEXUS_ASR_MODEL", "");
  const std::string precision =
      env_or_default("SLOTNEXUS_ASR_PRECISION", "fp32");
  const std::string fixture_dir =
      env_or_default("SLOTNEXUS_VOICE_FIXTURE_DIR", "");

  if (model_dir.empty()) {
    std::cout << "  [skip] 未配置 SLOTNEXUS_ASR_MODEL（板端 ctest 需 "
                 "-DSLOTNEXUS_ASR_MODEL=<模型目录>）" << std::endl;
    return 0;
  }
  if (precision != "fp32" && precision != "int8") {
    std::cerr << "  [fail] SLOTNEXUS_ASR_PRECISION 必须是 fp32 或 int8，实际: "
              << precision << std::endl;
    return 1;
  }
  if (fixture_dir.empty()) {
    std::cerr << "  [fail] 未配置 SLOTNEXUS_VOICE_FIXTURE_DIR；"
                 "demo_zh.wav 端到端回归必须提供固定 fixture" << std::endl;
    return 1;
  }
  const std::string model_wav = model_dir + "/test_wavs/0.wav";
  if (!file_exists(model_wav)) {
    std::cerr << "  [fail] 测试 WAV 不存在: " << model_wav << std::endl;
    return 1;
  }
  try {
    test_model_test_wav(model_dir, precision);
    test_demo_fixture(model_dir, precision, fixture_dir);
    test_cancel_suppresses_feed(model_dir, precision);
    test_session_reset_clears_cancel(model_dir, precision);
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
