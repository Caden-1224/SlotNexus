// SherpaKws：基于 sherpa-onnx C API 的流式关键词唤醒后端。
// Author: Caden
//
// 只封装“16-bit PCM 帧 → 是否触发关键词”的厂商调用；不拥有线程和唤醒
// 状态机。模型包文件名沿用关键词模型目录约定，模型目录和关键词文件路径
// 由配置提供。accept() 接收 16-bit PCM，内部转换为模型需要的 float 采样。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct SherpaOnnxKeywordSpotter;
struct SherpaOnnxOnlineStream;

namespace slotnexus::backend::sherpa_onnx {

struct KwsConfig {
  std::string model_dir;       // 模型目录（encoder/decoder/joiner/tokens）
  std::string keywords_file;   // 关键词定义文件
  float keywords_score = 1.5f;
  float keywords_threshold = 0.25f;
  int num_trailing_blanks = 1;
  int num_threads = 1;
};

class SherpaKws {
 public:
  SherpaKws() = default;
  ~SherpaKws();

  SherpaKws(const SherpaKws&) = delete;
  SherpaKws& operator=(const SherpaKws&) = delete;

  bool load(const KwsConfig& config);
  bool ready() const;
  std::string accept(const std::int16_t* samples, std::size_t count);
  void reset();

 private:
  const SherpaOnnxKeywordSpotter* spotter_ = nullptr;
  const SherpaOnnxOnlineStream* stream_ = nullptr;
  int sample_rate_ = 16000;
  bool has_audio_ = false;
};

}  // namespace slotnexus::backend::sherpa_onnx
