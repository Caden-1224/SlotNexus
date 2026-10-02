// MeloTtsBackend 实现：组合文本前端、ONNX/RKNN 推理引擎、流式重采样与
// Author: Caden
// ITtsBackend 状态机。
//
// 编排逻辑为板端已验证的 MeloTTS 推理路径：结果类型使用本后端的
// MeloStatus/MeloResult，事件出口使用 SlotNexus 的 BackendEvent（kPcm… + kDone）。
//
// 线程与资源：不创建后台线程；编码/解码在 synthesize 调用线程同步执行，
// PCM 回调也在同一线程逐帧发生（首块 PCM 早于整段合成完成即可交付）。
// cancel() 只触碰原子标志，可与 synthesize 并发。
#include "melotts_internal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "slotnexus/backend/backend_event.hpp"
#include "slotnexus/backend/melotts/melotts_tts_backend.hpp"

namespace slotnexus::backend::melotts {
namespace {

constexpr std::size_t kGVectorFloats = 256U;
constexpr std::size_t kGVectorBytes = kGVectorFloats * sizeof(float);

bool is_regular_readable_file(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  std::ifstream input(path, std::ios::binary);
  return input.good();
}

MeloResult<std::vector<float>> load_g_vector(const std::string& path) {
  if (!is_regular_readable_file(path)) {
    return MeloResult<std::vector<float>>::failure(MeloCode::kInvalidInput,
                                                  "MeloTTS g 文件不可读");
  }
  std::ifstream input(path, std::ios::binary);
  std::vector<float> values(kGVectorFloats, 0.0F);
  input.read(reinterpret_cast<char*>(values.data()),
             static_cast<std::streamsize>(kGVectorBytes));
  if (input.gcount() != static_cast<std::streamsize>(kGVectorBytes) ||
      input.peek() != std::char_traits<char>::eof()) {
    return MeloResult<std::vector<float>>::failure(
        MeloCode::kInvalidInput, "MeloTTS g 文件必须恰好是 1024 字节");
  }
  return MeloResult<std::vector<float>>::success(std::move(values));
}

std::size_t range_sum(const std::vector<std::size_t>& values, std::size_t begin,
                      std::size_t end) {
  std::size_t total = 0;
  for (std::size_t index = begin; index < end; ++index) {
    total += values[index];
  }
  return total;
}

MeloResult<std::vector<std::size_t>> make_word_frame_counts(
    const MeloPhoneChunk& chunk, const std::vector<std::int32_t>& phone_lengths) {
  if (phone_lengths.size() != chunk.phones.size()) {
    return MeloResult<std::vector<std::size_t>>::failure(
        MeloCode::kBackendFailure, "MeloTTS phone_lengths 与输入长度不一致");
  }
  std::vector<std::size_t> counts;
  counts.reserve(chunk.word_phone_counts.size());
  std::size_t phone_index = 0;
  for (const std::size_t phone_count : chunk.word_phone_counts) {
    if (phone_count == 0U) {
      counts.push_back(0U);
      continue;
    }
    if (phone_index + phone_count > phone_lengths.size()) {
      return MeloResult<std::vector<std::size_t>>::failure(
          MeloCode::kBackendFailure, "MeloTTS word_phone_counts 越界");
    }
    std::size_t frames = 0;
    for (std::size_t index = 0; index < phone_count; ++index) {
      const std::int32_t length = phone_lengths[phone_index + index];
      if (length < 0) {
        return MeloResult<std::vector<std::size_t>>::failure(
            MeloCode::kBackendFailure, "MeloTTS 预测出负的发音时长");
      }
      frames += static_cast<std::size_t>(length);
    }
    counts.push_back(frames);
    phone_index += phone_count;
  }
  if (phone_index != phone_lengths.size()) {
    return MeloResult<std::vector<std::size_t>>::failure(
        MeloCode::kBackendFailure, "MeloTTS word_phone_counts 未覆盖全部音素");
  }
  return MeloResult<std::vector<std::size_t>>::success(std::move(counts));
}

bool finite_non_negative(float value) {
  return std::isfinite(value) && value >= 0.0F;
}

// 一次 synthesize 的可审计统计（写入节点日志，作为合成侧证据）。
struct MeloStats {
  std::size_t input_text_bytes = 0;
  std::size_t text_chunks = 0;
  std::size_t unknown_units = 0;
  std::size_t encoder_runs = 0;
  std::size_t decoder_runs = 0;
  std::size_t native_samples = 0;
  std::size_t resampled_samples = 0;
  std::size_t delivered_frames = 0;
  std::size_t last_frame_valid_samples = 0;
  const char* tail_kind = "none";
  double elapsed_ms = 0.0;
  // 阶段耗时用于定位 TTS 瓶颈：文本前端、ONNX 编码器、RKNN 解码器与重采样
  // 分别累计；first_pcm_ms 是从 synthesize 开始到首个 PCM 事件的墙钟时间。
  double frontend_ms = 0.0;
  double encode_ms = 0.0;
  double decode_ms = 0.0;
  double convert_ms = 0.0;
  double first_pcm_ms = -1.0;
};

double elapsed_ms_since(std::chrono::steady_clock::time_point begin) {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - begin)
      .count();
}

}  // namespace

// 按帧连续切片：每个切片覆盖 frames_per_call 帧（末尾取余），相邻切片无重叠，
// 因此没有重复解码；实测比按词单元 + 2 单元上下文的切法少约 20% Decoder 调用。
// 解码器对任意 [offset, offset+count) 都支持，右端不足时由内部补零。
MeloResult<std::vector<DecodeSlice>> build_decode_slices(
    const std::vector<std::size_t>& unit_frames, std::size_t frames_per_call) {
  if (frames_per_call == 0U) {
    return MeloResult<std::vector<DecodeSlice>>::failure(
        MeloCode::kBackendFailure, "MeloTTS 解码器单次帧数预算为 0");
  }
  const std::size_t total_frames =
      range_sum(unit_frames, 0U, unit_frames.size());
  if (total_frames == 0U) {
    return MeloResult<std::vector<DecodeSlice>>::failure(
        MeloCode::kBackendFailure, "MeloTTS 编码器没有产生可解码帧");
  }
  std::vector<DecodeSlice> slices;
  slices.reserve((total_frames + frames_per_call - 1U) / frames_per_call);
  for (std::size_t begin = 0U; begin < total_frames; begin += frames_per_call) {
    const std::size_t end = std::min(begin + frames_per_call, total_frames);
    slices.push_back(DecodeSlice{begin, end});
  }
  return MeloResult<std::vector<DecodeSlice>>::success(std::move(slices));
}

struct MeloTtsBackend::Impl {
  explicit Impl(MeloTtsConfig cfg) : config(std::move(cfg)) {}

  // 构造期一次性校验 + 加载；失败消息写入 error。
  bool initialize(std::string* error) {
    if (config.encoder_model_path.empty() || config.decoder_model_path.empty() ||
        config.lexicon_path.empty() || config.tokens_path.empty() ||
        config.g_path.empty()) {
      *error = "MeloTTS 模型/文本资源路径为空";
      return false;
    }
    if (config.encoder_runtime != MeloRuntime::kOnnxRuntimeCpu ||
        config.decoder_runtime != MeloRuntime::kRknnNpu) {
      *error = "MeloTTS 只支持 ONNX Runtime CPU 编码器 + RKNN NPU 解码器";
      return false;
    }
    if (config.native_sample_rate_hz == 0U) {
      *error = "MeloTTS 原生采样率为 0";
      return false;
    }
    if (!std::isfinite(config.speed) || config.speed <= 0.0F ||
        !finite_non_negative(config.noise_scale) ||
        !finite_non_negative(config.noise_scale_w) ||
        !finite_non_negative(config.sdp_ratio)) {
      *error = "MeloTTS 推理参数非法";
      return false;
    }
    if (config.max_text_bytes == 0U || config.max_encoder_phones < 3U ||
        config.max_z_frames_per_chunk == 0U) {
      *error = "MeloTTS 容量参数非法";
      return false;
    }

    auto g_values = load_g_vector(config.g_path);
    if (!g_values.ok()) {
      *error = g_values.message;
      return false;
    }
    g = std::move(g_values.value);

    auto loaded_frontend =
        MeloTextFrontend::create(config.lexicon_path, config.tokens_path);
    if (!loaded_frontend.ok()) {
      *error = loaded_frontend.message;
      return false;
    }
    frontend = std::move(loaded_frontend.value);

    MeloOrtEncoderConfig encoder_config;
    encoder_config.model_path = config.encoder_model_path;
    encoder_config.intra_op_num_threads = config.intra_op_num_threads;
    auto created_encoder = MeloOrtEncoder::create(encoder_config);
    if (!created_encoder.ok()) {
      *error = created_encoder.message;
      return false;
    }
    encoder = std::move(created_encoder.value);

    MeloRknnDecoderConfig decoder_config;
    decoder_config.model_path = config.decoder_model_path;
    decoder_config.run_timeout_ms = config.run_timeout_ms;
    decoder_config.core_mask = config.decoder_core_mask;
    auto created_decoder = MeloRknnDecoder::create(decoder_config);
    if (!created_decoder.ok()) {
      *error = created_decoder.message;
      return false;
    }
    decoder = std::move(created_decoder.value);

    const MeloDecoderInfo& decoder_info = decoder->info();
    if (decoder_info.channels_per_frame == 0U || decoder_info.frames_per_call == 0U ||
        decoder_info.samples_per_frame == 0U) {
      *error = "MeloTTS 解码器形状信息非法";
      return false;
    }
    std::cerr << "[melotts] 解码器形状 channels_per_frame="
              << decoder_info.channels_per_frame
              << " frames_per_call=" << decoder_info.frames_per_call
              << " samples_per_frame=" << decoder_info.samples_per_frame << std::endl;
    return true;
  }

  // 交付帧缓冲区中的完整 320 采样帧；flush_tail 为真时把不足 320 的尾帧补零后
  // 交付并更新尾部统计。返回 false 表示已取消。
  bool emit_frames(std::vector<std::int16_t>& frame_buffer, bool flush_tail) {
    while (frame_buffer.size() >= static_cast<std::size_t>(kFrameSamples)) {
      if (cancelled.load()) {
        return false;
      }
      std::vector<std::int16_t> frame(
          frame_buffer.begin(),
          frame_buffer.begin() + static_cast<std::ptrdiff_t>(kFrameSamples));
      if (stats.first_pcm_ms < 0.0) {
        stats.first_pcm_ms = elapsed_ms_since(synth_begin);
      }
      cb(BackendEvent{BackendEvent::Kind::kPcm, {}, std::move(frame)});
      ++stats.delivered_frames;
      frame_buffer.erase(frame_buffer.begin(),
                         frame_buffer.begin() +
                             static_cast<std::ptrdiff_t>(kFrameSamples));
    }

    if (flush_tail && !frame_buffer.empty()) {
      if (cancelled.load()) {
        return false;
      }
      const std::size_t valid_samples = frame_buffer.size();
      std::vector<std::int16_t> frame = std::move(frame_buffer);
      frame.resize(static_cast<std::size_t>(kFrameSamples), 0);
      if (stats.first_pcm_ms < 0.0) {
        stats.first_pcm_ms = elapsed_ms_since(synth_begin);
      }
      cb(BackendEvent{BackendEvent::Kind::kPcm, {}, std::move(frame)});
      ++stats.delivered_frames;
      frame_buffer.clear();
      stats.last_frame_valid_samples = valid_samples;
      stats.tail_kind = "padded";
    }

    if (flush_tail && stats.delivered_frames > 0U && frame_buffer.empty() &&
        std::strcmp(stats.tail_kind, "none") == 0) {
      if (stats.resampled_samples % static_cast<std::size_t>(kFrameSamples) == 0U) {
        stats.last_frame_valid_samples = static_cast<std::size_t>(kFrameSamples);
        stats.tail_kind = "full";
      }
    }
    return true;
  }

  // 合成主循环；成功返回 true，失败返回 false 并写入 error。
  bool run_synthesis(const std::string& text, std::string* error) {
    const auto frontend_begin = std::chrono::steady_clock::now();
    auto chunks = frontend.convert(text, config.max_encoder_phones);
    stats.frontend_ms += elapsed_ms_since(frontend_begin);
    if (!chunks.ok()) {
      *error = chunks.message;
      return false;
    }
    if (chunks.value.empty()) {
      *error = "MeloTTS 文本前端没有产生片段";
      return false;
    }
    stats.text_chunks = chunks.value.size();

    const MeloDecoderInfo& decoder_info = decoder->info();
    MeloTtsPcmConverter converter(config.native_sample_rate_hz,
                                  static_cast<std::uint32_t>(kSampleRateHz));
    if (!converter.valid()) {
      *error = "MeloTTS 重采样采样率非法";
      return false;
    }

    std::vector<std::int16_t> frame_buffer;
    frame_buffer.reserve(static_cast<std::size_t>(kFrameSamples));

    for (const MeloPhoneChunk& chunk : chunks.value) {
      if (cancelled.load()) {
        *error = "MeloTTS 合成在文本片段间取消";
        return false;
      }
      if (chunk.phones.size() != chunk.tones.size() ||
          chunk.phones.size() != chunk.languages.size()) {
        *error = "MeloTTS 文本片段张量长度不一致";
        return false;
      }
      stats.unknown_units += chunk.unknown_units;

      MeloEncoderRequest request;
      request.phones = chunk.phones.data();
      request.phone_count = chunk.phones.size();
      request.tones = chunk.tones.data();
      request.tone_count = chunk.tones.size();
      request.languages = chunk.languages.data();
      request.language_count = chunk.languages.size();
      request.g = g.data();
      request.g_count = g.size();
      request.noise_scale = config.noise_scale;
      request.noise_scale_w = config.noise_scale_w;
      request.length_scale = 1.0F / config.speed;
      request.sdp_ratio = config.sdp_ratio;

      MeloEncoderResponse response;
      const auto encode_begin = std::chrono::steady_clock::now();
      const MeloStatus encoded = encoder->run(request, response);
      stats.encode_ms += elapsed_ms_since(encode_begin);
      if (!encoded.ok()) {
        *error = encoded.message;
        return false;
      }
      ++stats.encoder_runs;
      if (response.channels != decoder_info.channels_per_frame ||
          response.channels == 0U || response.frames == 0U ||
          response.z_p.size() != response.channels * response.frames ||
          response.phone_lengths.size() != chunk.phones.size()) {
        *error = "MeloTTS 编码器输出形状与解码器不匹配";
        return false;
      }
      if (response.frames > config.max_z_frames_per_chunk) {
        *error = "MeloTTS 编码器输出超过 z_p 帧预算";
        return false;
      }

      auto word_frame_counts = make_word_frame_counts(chunk, response.phone_lengths);
      if (!word_frame_counts.ok()) {
        *error = word_frame_counts.message;
        return false;
      }
      const std::size_t total_frames =
          range_sum(word_frame_counts.value, 0U, word_frame_counts.value.size());
      if (total_frames != response.frames) {
        *error = "MeloTTS 发音时长之和与 z_p 帧数不一致";
        return false;
      }
      const std::size_t expected_native_samples =
          total_frames * decoder_info.samples_per_frame;
      if (response.audio_len_samples < 0 ||
          static_cast<std::size_t>(response.audio_len_samples) !=
              expected_native_samples) {
        *error = "MeloTTS 原生采样数与时长相位不一致";
        return false;
      }

      auto slices =
          build_decode_slices(word_frame_counts.value, decoder_info.frames_per_call);
      if (!slices.ok()) {
        *error = slices.message;
        return false;
      }

      std::size_t previous_frame_end = 0U;
      std::size_t slice_index = 0U;
      for (const DecodeSlice& slice : slices.value) {
        if (cancelled.load()) {
          *error = "MeloTTS 合成在解码切片间取消";
          return false;
        }
        if (slice.frame_begin > previous_frame_end ||
            slice.frame_end < previous_frame_end) {
          *error = "MeloTTS 解码切片覆盖关系非法";
          return false;
        }
        const std::size_t frame_count = slice.frame_end - slice.frame_begin;
        if (frame_count == 0U || frame_count > decoder_info.frames_per_call) {
          *error = "MeloTTS 解码切片帧数非法";
          return false;
        }

        std::vector<float> native_audio;
        const auto decode_begin = std::chrono::steady_clock::now();
        const MeloStatus decoded =
            decoder->decode(response.z_p.data(), response.frames, slice.frame_begin,
                            frame_count, native_audio);
        stats.decode_ms += elapsed_ms_since(decode_begin);
        if (!decoded.ok()) {
          *error = decoded.message;
          return false;
        }
        ++stats.decoder_runs;
        if (native_audio.size() != frame_count * decoder_info.samples_per_frame) {
          *error = "MeloTTS 解码器返回样本数不符合形状";
          return false;
        }
        if (cancelled.load()) {
          *error = "MeloTTS 合成在解码后取消";
          return false;
        }

        std::size_t drop_samples = 0U;
        if (slice_index > 0U) {
          if (previous_frame_end < slice.frame_begin) {
            *error = "MeloTTS 解码切片没有连续覆盖 z_p";
            return false;
          }
          drop_samples = (previous_frame_end - slice.frame_begin) *
                         decoder_info.samples_per_frame;
        }
        if (drop_samples > native_audio.size()) {
          *error = "MeloTTS 重叠样本数超过切片长度";
          return false;
        }
        const std::size_t keep_samples = native_audio.size() - drop_samples;
        if (keep_samples > 0U) {
          stats.native_samples += keep_samples;
          std::vector<std::int16_t> converted;
          const auto convert_begin = std::chrono::steady_clock::now();
          converter.push(native_audio.data() + drop_samples, keep_samples, converted);
          stats.convert_ms += elapsed_ms_since(convert_begin);
          frame_buffer.insert(frame_buffer.end(), converted.begin(), converted.end());
          if (!emit_frames(frame_buffer, false)) {
            *error = "MeloTTS 回调取消";
            return false;
          }
        }
        previous_frame_end = std::max(previous_frame_end, slice.frame_end);
        ++slice_index;
      }
      if (previous_frame_end != response.frames) {
        *error = "MeloTTS 解码切片没有覆盖全部 z_p 帧";
        return false;
      }
    }

    if (cancelled.load()) {
      *error = "MeloTTS 合成完成前取消";
      return false;
    }
    std::vector<std::int16_t> tail_samples;
    converter.flush(tail_samples);
    stats.resampled_samples = converter.total_output_samples();
    frame_buffer.insert(frame_buffer.end(), tail_samples.begin(), tail_samples.end());
    if (!emit_frames(frame_buffer, true)) {
      *error = "MeloTTS 尾帧回调取消";
      return false;
    }
    if (cancelled.load()) {
      *error = "MeloTTS 尾帧后取消";
      return false;
    }
    if (stats.delivered_frames == 0U || stats.resampled_samples == 0U) {
      *error = "MeloTTS 没有产生可交付 PCM";
      return false;
    }
    return true;
  }

  MeloTtsConfig config;
  std::vector<float> g;
  MeloTextFrontend frontend;
  std::unique_ptr<IMeloEncoder> encoder;
  std::unique_ptr<IMeloDecoder> decoder;
  EventCallback cb;
  std::atomic<bool> cancelled{false};
  MeloStats stats{};
  // synthesize 起点，用于首个 PCM 的墙钟时间；不在统计重置时清零。
  std::chrono::steady_clock::time_point synth_begin{};
};

MeloTtsBackend::MeloTtsBackend(MeloTtsConfig config) {
  auto impl = std::make_unique<Impl>(std::move(config));
  std::string error;
  if (!impl->initialize(&error)) {
    throw std::runtime_error("MeloTTS 初始化失败: " + error);
  }
  impl_ = std::move(impl);
}

MeloTtsBackend::~MeloTtsBackend() = default;

void MeloTtsBackend::set_event_callback(EventCallback cb) {
  impl_->cb = std::move(cb);
  impl_->cancelled.store(false);
  impl_->stats = MeloStats{};
}

void MeloTtsBackend::synthesize(const std::string& text) {
  if (!impl_->cb || impl_->cancelled.load()) {
    return;
  }
  impl_->stats = MeloStats{};
  impl_->stats.input_text_bytes = text.size();
  if (text.empty()) {
    std::cerr << "[melotts] 输入文本为空，未产出事件" << std::endl;
    return;
  }
  if (text.size() > impl_->config.max_text_bytes) {
    std::cerr << "[melotts] 文本超过 max_text_bytes=" << impl_->config.max_text_bytes
              << "，拒绝静默截断" << std::endl;
    return;
  }

  const auto begin = std::chrono::steady_clock::now();
  impl_->synth_begin = begin;
  std::string error;
  const bool ok = impl_->run_synthesis(text, &error);
  const auto end = std::chrono::steady_clock::now();
  impl_->stats.elapsed_ms =
      std::chrono::duration<double, std::milli>(end - begin).count();

  if (!ok) {
    // 协议无错误事件：失败时不产出任何事件，仅记录到节点日志。
    std::cerr << "[melotts] 合成失败: " << error << std::endl;
    return;
  }
  const double audio_s = static_cast<double>(impl_->stats.resampled_samples) /
                         static_cast<double>(kSampleRateHz);
  std::cerr << "[melotts] 合成完成 text_bytes=" << impl_->stats.input_text_bytes
            << " chunks=" << impl_->stats.text_chunks
            << " unknown_units=" << impl_->stats.unknown_units
            << " encoder_runs=" << impl_->stats.encoder_runs
            << " decoder_runs=" << impl_->stats.decoder_runs
            << " native_samples=" << impl_->stats.native_samples
            << " resampled_samples=" << impl_->stats.resampled_samples
            << " frames=" << impl_->stats.delivered_frames
            << " tail=" << impl_->stats.tail_kind
            << " last_frame_valid=" << impl_->stats.last_frame_valid_samples
            << " audio_s=" << audio_s
            << " elapsed_ms=" << impl_->stats.elapsed_ms
            << " frontend_ms=" << impl_->stats.frontend_ms
            << " encode_ms=" << impl_->stats.encode_ms
            << " decode_ms=" << impl_->stats.decode_ms
            << " convert_ms=" << impl_->stats.convert_ms
            << " first_pcm_ms=" << impl_->stats.first_pcm_ms
            << " rtf=" << (audio_s > 0.0 ? impl_->stats.elapsed_ms / 1000.0 / audio_s
                                         : 0.0)
            << std::endl;

  if (!impl_->cancelled.load() && impl_->cb) {
    impl_->cb(BackendEvent{BackendEvent::Kind::kDone, {}, {}});
  }
}

void MeloTtsBackend::cancel() { impl_->cancelled.store(true); }

}  // namespace slotnexus::backend::melotts
