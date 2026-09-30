// 常驻音频输入分段：把 20 ms 帧持续交给能量 VAD 和流式 ASR。
// Author: Caden
//
// 职责：只做“帧 → 话语边界 → ASR 会话”的编排，不访问声卡、线程或模型。
// 调用方按到达顺序 feed_audio()；达到最小静音时，当前帧作为 ASR 的
// is_last 帧收尾，kFinal 文本通过回调返回。静音期保留短前置缓冲，确保
// 起音前的少量帧不被丢掉。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "slotnexus/backend/i_asr_backend.hpp"

namespace slotnexus::session {

class StreamingInput {
 public:
  struct Config {
    std::size_t frame_samples = 320;   // 20 ms @16 kHz
    std::size_t pre_roll_frames = 10;  // 起音前保留 200 ms
    std::size_t min_speech_frames = 1; // 连续多少帧算起音
    std::size_t min_silence_frames = 10;  // 连续多少帧算话语结束
    int speech_rms_threshold = 250;    // 未注入 VAD 时的 16-bit RMS 阈值
    // 可选人声判定（如 Silero VAD）；为空时退回 RMS 能量阈值。
    std::function<bool(const std::vector<int16_t>&)> speech_detector;
  };

  using FinalCallback = std::function<void(std::string)>;
  using PartialCallback = std::function<void(std::string)>;
  using EndpointCallback = std::function<void()>;
  using SpeechStartedCallback = std::function<void()>;

  StreamingInput(Config config, backend::IAsrBackend& asr);

  StreamingInput(const StreamingInput&) = delete;
  StreamingInput& operator=(const StreamingInput&) = delete;

  // 设置本轮/后续所有话语的事件回调；可在 feed_audio 前设置一次。
  void set_callbacks(FinalCallback on_final,
                     PartialCallback on_partial = {},
                     EndpointCallback on_endpoint = {},
                     SpeechStartedCallback on_speech_started = {});

  // 追加 PCM；内部按 frame_samples 切帧，不要求调用方恰好按帧喂入。
  void feed_audio(const int16_t* samples, std::size_t count);

  // 结束输入：若当前话语尚未遇到静音收尾，用空尾帧强制 ASR kFinal。
  void flush();

  // 放弃当前话语并重置 VAD/缓冲；下一次 feed 从静音态重新开始。
  void reset();

 private:
  void process_frame(const std::vector<int16_t>& frame);
  bool is_speech(const std::vector<int16_t>& frame) const;
  void begin_utterance();
  void end_utterance();

  Config config_;
  backend::IAsrBackend& asr_;
  FinalCallback on_final_;
  PartialCallback on_partial_;
  EndpointCallback on_endpoint_;
  SpeechStartedCallback on_speech_started_;

  std::vector<int16_t> pending_samples_;
  std::vector<std::vector<int16_t>> pre_roll_;
  std::size_t speech_run_ = 0;
  std::size_t silence_run_ = 0;
  bool active_ = false;
};

}  // namespace slotnexus::session
