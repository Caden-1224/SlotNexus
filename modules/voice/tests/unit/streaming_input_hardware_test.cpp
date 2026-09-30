// StreamingInput + 真实 Sherpa ASR 的板端分段验证。
// Author: Caden
//
// 用固定 WAV 帧驱动能量 VAD：WAV 播放完后追加 500 ms 静音，验证
// StreamingInput 能判停、产生 final，并记录“端点确认 → ASR final”耗时。
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "slotnexus/backend/backend_event.hpp"
#include "slotnexus/backend/sherpa_onnx/sherpa_asr_backend.hpp"
#include "slotnexus/backend/sherpa_onnx/sherpa_vad.hpp"
#include "slotnexus/common/wav_reader.hpp"
#include "slotnexus/session/streaming_input.hpp"

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

}  // namespace

int main() {
  const std::string model_dir = env_or_default("SLOTNEXUS_ASR_MODEL", "");
  const std::string precision = env_or_default("SLOTNEXUS_ASR_PRECISION", "fp32");
  const std::string fixture_dir =
      env_or_default("SLOTNEXUS_VOICE_FIXTURE_DIR", "");
  if (model_dir.empty() || fixture_dir.empty()) {
    std::cout << "streaming_input_hardware_test: 跳过（未配置 ASR 模型/fixture）"
              << std::endl;
    return 0;
  }

  const std::string wav_path = fixture_dir + "/demo_zh.wav";
  const auto wav = WavReader::read(wav_path);
  CHECK(wav.ok);
  if (!wav.ok) {
    std::cerr << "无法读取 fixture WAV: " << wav_path << " (" << wav.error
              << ")" << std::endl;
    return 1;
  }

  es::SherpaAsrBackend asr(model_dir, /*num_threads=*/4, precision);
  slotnexus::session::StreamingInput::Config config;
  config.frame_samples = static_cast<std::size_t>(eb::kFrameSamples);
  config.pre_roll_frames = 25;  // 500 ms
  config.min_speech_frames = 3; // 60 ms
  config.min_silence_frames = 25;  // 500 ms
  config.speech_rms_threshold = 100;

  const std::string vad_model =
      env_or_default("SLOTNEXUS_VAD_MODEL",
                     fixture_dir + "/../models/silero_vad.onnx");
  es::SherpaVad vad(vad_model);
  if (vad.ready()) {
    config.speech_detector = [&](const std::vector<int16_t>& frame) {
      return vad.is_speech(frame.data(), frame.size());
    };
    std::cout << "  [info] 使用 Silero VAD: " << vad_model << std::endl;
  } else {
    std::cout << "  [warn] Silero VAD 不可用，退回能量阈值: " << vad_model
              << std::endl;
  }

  slotnexus::session::StreamingInput input(config, asr);
  std::string final_text;
  std::chrono::steady_clock::time_point endpoint_time{};
  std::chrono::steady_clock::time_point final_time{};
  input.set_callbacks(
      [&](std::string text) {
        final_text = std::move(text);
        final_time = std::chrono::steady_clock::now();
      },
      [&] { endpoint_time = std::chrono::steady_clock::now(); });

  const auto feed_frame = [&](const std::vector<int16_t>& frame) {
    input.feed_audio(frame.data(), frame.size());
  };

  const auto& samples = wav.info.samples;
  std::size_t offset = 0;
  while (offset < samples.size()) {
    const std::size_t n = std::min<std::size_t>(
        static_cast<std::size_t>(eb::kFrameSamples), samples.size() - offset);
    std::vector<int16_t> frame(samples.begin() + offset,
                               samples.begin() + offset + n);
    offset += n;
    feed_frame(frame);
  }
  const std::vector<int16_t> silence(
      static_cast<std::size_t>(eb::kFrameSamples), 0);
  for (int i = 0; i < 30; ++i) {
    feed_frame(silence);
  }

  CHECK(!final_text.empty());
  CHECK(final_text == "你好这是语音合成测试");
  CHECK(endpoint_time.time_since_epoch().count() != 0);
  CHECK(final_time >= endpoint_time);
  const auto endpoint_to_final_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(final_time -
                                                            endpoint_time)
          .count();
  std::printf("  [info] asr_final=\"%s\" endpoint_to_final_ms=%lld\n",
              final_text.c_str(), static_cast<long long>(endpoint_to_final_ms));

  if (g_failures == 0) {
    std::cout << "streaming_input_hardware_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "streaming_input_hardware_test 失败 " << g_failures << " 项"
            << std::endl;
  return 1;
}
