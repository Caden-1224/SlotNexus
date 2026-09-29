// SherpaSileroVad：基于 sherpa-onnx C API 的 Silero VAD 前端。
// Author: Caden
//
// 只做“320 样本领域帧片段 → 512 样本模型窗口 → 当前是否人声”的转换；
// 不拥有话语状态机，起音/判停计数仍由 StreamingInput 负责。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SherpaOnnxVoiceActivityDetector;

namespace slotnexus::backend::sherpa_onnx {

class SherpaVad {
 public:
  explicit SherpaVad(std::string model_path, int sample_rate = 16000);
  ~SherpaVad();

  SherpaVad(const SherpaVad&) = delete;
  SherpaVad& operator=(const SherpaVad&) = delete;

  bool ready() const { return vad_ != nullptr; }

  // 追加一帧 16-bit 单声道；返回处理完当前输入后的 VAD 人声状态。
  bool is_speech(const std::int16_t* samples, std::size_t count);

  void reset();

 private:
  const SherpaOnnxVoiceActivityDetector* vad_ = nullptr;
  int sample_rate_ = 16000;
  std::vector<float> pending_;
};

}  // namespace slotnexus::backend::sherpa_onnx
