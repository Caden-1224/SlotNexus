// MeloTTS 后端内部类型：结果类型、文本前端、流式重采样与推理引擎接口。
// Author: Caden
//
// 本头文件只出现在 modules/voice/backends/melotts/src/ 内，不进入公共契约；ONNX Runtime 与
// RKNN 的厂商类型只在 melotts_real_engines.cpp 出现。
//
// 本文件只提供本后端需要的最小结果类型：加载/推理状态、消息与文本前端、
// 流式重采样和编解码引擎接口，不引入与 SlotNexus 语音契约无关的层级。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slotnexus::backend::melotts {

// ---------------------------------------------------------------------------
// 最小结果类型：表达一次加载/推理操作的状态与消息
// ---------------------------------------------------------------------------

enum class MeloCode {
  kOk = 0,
  kInvalidInput,
  kBackendFailure,
  kDeviceFailure,
  kTimeout,
  kCancelled,
};

struct MeloStatus {
  MeloCode code = MeloCode::kOk;
  std::string message;

  bool ok() const noexcept { return code == MeloCode::kOk; }

  static MeloStatus success() { return {}; }
  static MeloStatus failure(MeloCode code, std::string message) {
    MeloStatus status;
    status.code = code;
    status.message = std::move(message);
    return status;
  }
};

template <class T>
struct MeloResult {
  MeloCode code = MeloCode::kOk;
  std::string message;
  T value{};

  bool ok() const noexcept { return code == MeloCode::kOk; }

  static MeloResult success(T value) {
    MeloResult result;
    result.value = std::move(value);
    return result;
  }
  static MeloResult failure(MeloCode code, std::string message) {
    MeloResult result;
    result.code = code;
    result.message = std::move(message);
    return result;
  }
};

// ---------------------------------------------------------------------------
// 文本前端：UTF-8 文本 → phone/tone/language 张量
// ---------------------------------------------------------------------------

// 一个可直接送入编码器的文本片段。phones/tones/languages 已按 MeloTTS VITS
// 约定在音素之间插入 blank；word_phone_counts 是 interspersed 序列上的分组
// 长度，求和必须等于 phones.size()。
struct MeloPhoneChunk {
  std::vector<std::int32_t> phones;
  std::vector<std::int32_t> tones;
  std::vector<std::int32_t> languages;
  std::vector<std::size_t> word_phone_counts;
  std::size_t unknown_units = 0;
};

class MeloTextFrontend {
 public:
  static MeloResult<MeloTextFrontend> create(const std::string& lexicon_path,
                                             const std::string& tokens_path);

  MeloTextFrontend() = default;

  // max_encoder_phones 是 interspersed phone 数上限，必须 >= 3。
  MeloResult<std::vector<MeloPhoneChunk>> convert(
      const std::string& text, std::size_t max_encoder_phones) const;

  std::size_t lexicon_entry_count() const noexcept { return lexicon_.size(); }

 private:
  struct Unit {
    std::vector<std::int32_t> phones;
    std::vector<std::int32_t> tones;
  };

  static MeloResult<MeloTextFrontend> load(const std::string& lexicon_path,
                                           const std::string& tokens_path);

  std::unordered_map<std::string, Unit> lexicon_;
  std::unordered_map<std::string, std::int32_t> token_ids_;
  std::int32_t blank_id_ = 0;
  std::int32_t unk_id_ = 0;
  std::size_t max_key_codepoints_ = 1;
  bool loaded_ = false;
};

// ---------------------------------------------------------------------------
// 流式线性重采样：原生 float 单声道 → 16 kHz int16
// ---------------------------------------------------------------------------

class MeloTtsPcmConverter {
 public:
  MeloTtsPcmConverter(std::uint32_t input_rate_hz, std::uint32_t output_rate_hz);

  bool valid() const noexcept { return input_rate_hz_ > 0 && output_rate_hz_ > 0; }

  void push(const float* samples, std::size_t count, std::vector<std::int16_t>& output);
  void flush(std::vector<std::int16_t>& output);

  std::size_t total_input_samples() const noexcept { return total_input_samples_; }
  std::size_t total_output_samples() const noexcept { return next_output_index_; }

 private:
  void drain(std::size_t target, std::vector<std::int16_t>& output);
  bool can_emit_next() const noexcept;
  float sample_at(std::size_t global_index) const noexcept;
  void compact_before(std::size_t global_index);

  std::uint32_t input_rate_hz_ = 0;
  std::uint32_t output_rate_hz_ = 0;
  double ratio_ = 0.0;  // input_rate_hz_ / output_rate_hz_。
  std::vector<float> input_;
  std::size_t input_base_ = 0;
  std::size_t total_input_samples_ = 0;
  std::size_t next_output_index_ = 0;
  bool flushed_ = false;
};

// ---------------------------------------------------------------------------
// 推理引擎接口
// ---------------------------------------------------------------------------

struct MeloEncoderRequest {
  const std::int32_t* phones = nullptr;
  std::size_t phone_count = 0;
  const std::int32_t* tones = nullptr;
  std::size_t tone_count = 0;
  const std::int32_t* languages = nullptr;
  std::size_t language_count = 0;
  const float* g = nullptr;
  std::size_t g_count = 0;
  float noise_scale = 0.0f;
  float noise_scale_w = 0.0f;
  float length_scale = 1.0f;
  float sdp_ratio = 0.0f;
};

struct MeloEncoderResponse {
  std::vector<float> z_p;  // [channels][frames] 连续存放
  std::size_t channels = 0;
  std::size_t frames = 0;
  std::vector<std::int32_t> phone_lengths;
  std::int32_t audio_len_samples = 0;
};

class IMeloEncoder {
 public:
  virtual ~IMeloEncoder() = default;
  virtual MeloStatus run(const MeloEncoderRequest& request,
                         MeloEncoderResponse& response) = 0;
};

struct MeloDecoderInfo {
  std::size_t channels_per_frame = 0;
  std::size_t frames_per_call = 0;
  std::size_t samples_per_frame = 0;
};

// 一次 RKNN 解码要覆盖的 z_p 帧区间；切片按帧连续覆盖 [0,total)，无重叠。
struct DecodeSlice {
  std::size_t frame_begin = 0;
  std::size_t frame_end = 0;
};

// 把单词级帧数合计成不超过 frames_per_call 的连续帧切片；run 数等于
// ceil(total/frames_per_call)。空输入或预算为 0 返回失败。
MeloResult<std::vector<DecodeSlice>> build_decode_slices(
    const std::vector<std::size_t>& unit_frames, std::size_t frames_per_call);

class IMeloDecoder {
 public:
  virtual ~IMeloDecoder() = default;
  virtual const MeloDecoderInfo& info() const noexcept = 0;
  virtual MeloStatus decode(const float* z_p, std::size_t total_z_frames,
                            std::size_t frame_offset, std::size_t frame_count,
                            std::vector<float>& audio) = 0;
};

// ONNX Runtime CPU 编码器。
struct MeloOrtEncoderConfig {
  std::string model_path;
  int intra_op_num_threads = 1;
};

class MeloOrtEncoder final : public IMeloEncoder {
 public:
  static MeloResult<std::unique_ptr<MeloOrtEncoder>> create(
      const MeloOrtEncoderConfig& config);
  ~MeloOrtEncoder() override;

  MeloOrtEncoder(const MeloOrtEncoder&) = delete;
  MeloOrtEncoder& operator=(const MeloOrtEncoder&) = delete;

  MeloStatus run(const MeloEncoderRequest& request,
                 MeloEncoderResponse& response) override;

 private:
  struct Impl;
  explicit MeloOrtEncoder(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// RKNN NPU 解码器。
struct MeloRknnDecoderConfig {
  std::string model_path;
  std::uint32_t run_timeout_ms = 30000u;
  // rknn_core_mask；0 = AUTO，1/2/4 = core0/1/2，按位组合。
  int core_mask = 0;
};

class MeloRknnDecoder final : public IMeloDecoder {
 public:
  static MeloResult<std::unique_ptr<MeloRknnDecoder>> create(
      const MeloRknnDecoderConfig& config);
  ~MeloRknnDecoder() override;

  MeloRknnDecoder(const MeloRknnDecoder&) = delete;
  MeloRknnDecoder& operator=(const MeloRknnDecoder&) = delete;

  const MeloDecoderInfo& info() const noexcept override;
  MeloStatus decode(const float* z_p, std::size_t total_z_frames,
                    std::size_t frame_offset, std::size_t frame_count,
                    std::vector<float>& audio) override;

 private:
  struct Impl;
  explicit MeloRknnDecoder(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace slotnexus::backend::melotts
