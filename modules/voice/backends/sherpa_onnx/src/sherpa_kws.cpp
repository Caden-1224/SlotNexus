#include "slotnexus/backend/sherpa_onnx/sherpa_kws.hpp"
// Author: Caden

#include <cstring>
#include <vector>

#include "sherpa-onnx/c-api/c-api.h"

namespace slotnexus::backend::sherpa_onnx {

namespace {
constexpr const char* kEncoderFile =
    "encoder-epoch-12-avg-2-chunk-16-left-64.onnx";
constexpr const char* kDecoderFile =
    "decoder-epoch-12-avg-2-chunk-16-left-64.onnx";
constexpr const char* kJoinerFile =
    "joiner-epoch-12-avg-2-chunk-16-left-64.onnx";

std::string join_path(const std::string& dir, const char* file) {
  if (dir.empty()) {
    return {};
  }
  if (dir.back() == '/') {
    return dir + file;
  }
  return dir + "/" + file;
}
}  // namespace

SherpaKws::~SherpaKws() {
  if (stream_ != nullptr) {
    SherpaOnnxDestroyOnlineStream(stream_);
  }
  if (spotter_ != nullptr) {
    SherpaOnnxDestroyKeywordSpotter(spotter_);
  }
}

bool SherpaKws::load(const KwsConfig& config) {
  const std::string encoder = join_path(config.model_dir, kEncoderFile);
  const std::string decoder = join_path(config.model_dir, kDecoderFile);
  const std::string joiner = join_path(config.model_dir, kJoinerFile);
  const std::string tokens = join_path(config.model_dir, "tokens.txt");
  if (encoder.empty() || config.keywords_file.empty()) {
    return false;
  }

  SherpaOnnxKeywordSpotterConfig cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.feat_config.sample_rate = sample_rate_;
  cfg.feat_config.feature_dim = 80;
  cfg.model_config.transducer.encoder = encoder.c_str();
  cfg.model_config.transducer.decoder = decoder.c_str();
  cfg.model_config.transducer.joiner = joiner.c_str();
  cfg.model_config.tokens = tokens.c_str();
  cfg.model_config.num_threads = config.num_threads;
  cfg.model_config.provider = "cpu";
  cfg.max_active_paths = 4;
  cfg.num_trailing_blanks = config.num_trailing_blanks;
  cfg.keywords_score = config.keywords_score;
  cfg.keywords_threshold = config.keywords_threshold;
  cfg.keywords_file = config.keywords_file.c_str();

  spotter_ = SherpaOnnxCreateKeywordSpotter(&cfg);
  if (spotter_ == nullptr) {
    return false;
  }
  stream_ = SherpaOnnxCreateKeywordStream(spotter_);
  return stream_ != nullptr;
}

bool SherpaKws::ready() const {
  return spotter_ != nullptr && stream_ != nullptr;
}

std::string SherpaKws::accept(const std::int16_t* samples, std::size_t count) {
  if (!ready() || samples == nullptr || count == 0) {
    return {};
  }
  std::vector<float> floats(count);
  for (std::size_t i = 0; i < count; ++i) {
    floats[i] = static_cast<float>(samples[i]) / 32768.0f;
  }
  has_audio_ = true;
  SherpaOnnxOnlineStreamAcceptWaveform(stream_, sample_rate_, floats.data(),
                                       static_cast<int>(count));
  while (SherpaOnnxIsKeywordStreamReady(spotter_, stream_)) {
    SherpaOnnxDecodeKeywordStream(spotter_, stream_);
  }

  std::string keyword;
  const SherpaOnnxKeywordResult* result =
      SherpaOnnxGetKeywordResult(spotter_, stream_);
  if (result != nullptr && result->keyword != nullptr &&
      result->keyword[0] != '\0') {
    keyword = result->keyword;
    SherpaOnnxResetKeywordStream(spotter_, stream_);
  }
  if (result != nullptr) {
    SherpaOnnxDestroyKeywordResult(result);
  }
  return keyword;
}

void SherpaKws::reset() {
  if (!ready() || !has_audio_) {
    return;
  }
  SherpaOnnxDestroyOnlineStream(stream_);
  stream_ = SherpaOnnxCreateKeywordStream(spotter_);
  has_audio_ = false;
}

}  // namespace slotnexus::backend::sherpa_onnx
