#include "slotnexus/backend/sherpa_onnx/sherpa_vad.hpp"
// Author: Caden

#include <cstring>
#include <utility>

#include "sherpa-onnx/c-api/c-api.h"

namespace slotnexus::backend::sherpa_onnx {

namespace {
constexpr int kWindowSamples = 512;  // Silero VAD 固定窗口
}

SherpaVad::SherpaVad(std::string model_path, int sample_rate)
    : sample_rate_(sample_rate) {
  if (model_path.empty() || sample_rate_ <= 0) {
    return;
  }
  SherpaOnnxVadModelConfig cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.silero_vad.model = model_path.c_str();
  cfg.silero_vad.threshold = 0.5f;
  // 只做帧级人声判定；真正的话语判停窗口由 StreamingInput 的
  // min_silence_frames 控制，避免 VAD 内部再叠加一段静音等待。
  cfg.silero_vad.min_silence_duration = 0.25f;
  cfg.silero_vad.min_speech_duration = 0.05f;
  cfg.silero_vad.max_speech_duration = 20.0f;
  cfg.silero_vad.window_size = kWindowSamples;
  cfg.sample_rate = sample_rate_;
  cfg.num_threads = 1;
  cfg.provider = "cpu";
  vad_ = SherpaOnnxCreateVoiceActivityDetector(&cfg, 30.0f);
}

SherpaVad::~SherpaVad() {
  if (vad_ != nullptr) {
    SherpaOnnxDestroyVoiceActivityDetector(vad_);
  }
}

bool SherpaVad::is_speech(const std::int16_t* samples, std::size_t count) {
  if (vad_ == nullptr || samples == nullptr || count == 0) {
    return false;
  }
  pending_.reserve(pending_.size() + count);
  for (std::size_t i = 0; i < count; ++i) {
    pending_.push_back(static_cast<float>(samples[i]) / 32768.0f);
  }
  std::size_t consumed = 0;
  while (pending_.size() - consumed >= static_cast<std::size_t>(kWindowSamples)) {
    SherpaOnnxVoiceActivityDetectorAcceptWaveform(
        vad_, pending_.data() + consumed, kWindowSamples);
    consumed += kWindowSamples;
    // 丢弃 VAD 内部已切出的语音段，避免队列随输入增长。
    while (!SherpaOnnxVoiceActivityDetectorEmpty(vad_)) {
      const SherpaOnnxSpeechSegment* seg =
          SherpaOnnxVoiceActivityDetectorFront(vad_);
      if (seg != nullptr) {
        SherpaOnnxDestroySpeechSegment(seg);
      }
      SherpaOnnxVoiceActivityDetectorPop(vad_);
    }
  }
  if (consumed > 0) {
    pending_.erase(pending_.begin(),
                   pending_.begin() + static_cast<std::ptrdiff_t>(consumed));
  }
  return SherpaOnnxVoiceActivityDetectorDetected(vad_) != 0;
}

void SherpaVad::reset() {
  pending_.clear();
  if (vad_ != nullptr) {
    SherpaOnnxVoiceActivityDetectorReset(vad_);
  }
}

}  // namespace slotnexus::backend::sherpa_onnx
