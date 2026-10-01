#include "slotnexus/session/streaming_input.hpp"
// Author: Caden

#include <algorithm>
#include <cmath>
#include <utility>

namespace slotnexus::session {

StreamingInput::StreamingInput(Config config, backend::IAsrBackend& asr)
    : config_(config), asr_(asr) {}

void StreamingInput::set_callbacks(FinalCallback on_final,
                                   EndpointCallback on_endpoint,
                                   SpeechStartedCallback on_speech_started,
                                   ResumeDetectedCallback on_resume_detected) {
  on_final_ = std::move(on_final);
  on_endpoint_ = std::move(on_endpoint);
  on_speech_started_ = std::move(on_speech_started);
  on_resume_detected_ = std::move(on_resume_detected);
}

void StreamingInput::feed_audio(const int16_t* samples, std::size_t count) {
  if (samples == nullptr || count == 0) {
    return;
  }
  pending_samples_.insert(pending_samples_.end(), samples, samples + count);
  while (pending_samples_.size() >= config_.frame_samples) {
    std::vector<int16_t> frame(
        pending_samples_.begin(),
        pending_samples_.begin() +
            static_cast<std::ptrdiff_t>(config_.frame_samples));
    pending_samples_.erase(
        pending_samples_.begin(),
        pending_samples_.begin() +
            static_cast<std::ptrdiff_t>(config_.frame_samples));
    process_frame(frame);
  }
}

void StreamingInput::flush() {
  if (!pending_samples_.empty()) {
    process_frame(std::move(pending_samples_));
    pending_samples_.clear();
  }
  if (!active_) {
    return;
  }
  // 输入结束但还没等到静音：用空尾帧收尾，不把截断文本当成正常话语。
  asr_.feed_audio({}, true);
  end_utterance();
}

void StreamingInput::process_frame(const std::vector<int16_t>& frame) {
  const bool speech = is_speech(frame);
  if (!active_) {
    pre_roll_.push_back(frame);
    if (pre_roll_.size() > config_.pre_roll_frames) {
      pre_roll_.erase(pre_roll_.begin());
    }
    if (!speech) {
      speech_run_ = 0;
      return;
    }
    ++speech_run_;
    if (speech_run_ >= config_.min_speech_frames) {
      begin_utterance();
    }
    return;
  }

  if (speech) {
    silence_run_ = 0;
    ++active_speech_run_;
    if (!resume_reported_ && config_.resume_frames > 0 &&
        active_speech_run_ >= config_.resume_frames) {
      resume_reported_ = true;
      if (on_resume_detected_) {
        on_resume_detected_();
      }
    }
    asr_.feed_audio(frame, false);
    return;
  }

  active_speech_run_ = 0;
  ++silence_run_;
  if (silence_run_ >= config_.min_silence_frames) {
    if (on_endpoint_) {
      on_endpoint_();
    }
    asr_.feed_audio(frame, true);
    end_utterance();
    return;
  }
  asr_.feed_audio(frame, false);
}

bool StreamingInput::is_speech(const std::vector<int16_t>& frame) const {
  if (frame.empty()) {
    return false;
  }
  if (config_.speech_detector) {
    return config_.speech_detector(frame);
  }
  double sum = 0.0;
  for (const int16_t sample : frame) {
    const double value = static_cast<double>(sample);
    sum += value * value;
  }
  const double rms = std::sqrt(sum / static_cast<double>(frame.size()));
  return rms >= static_cast<double>(config_.speech_rms_threshold);
}

void StreamingInput::begin_utterance() {
  // 起音判定的这几帧也是连续人声的一部分；保留首帧计数，避免续说阈值
  // 因 begin_utterance 重置而晚一帧。
  const std::size_t start_frames = speech_run_;
  active_ = true;
  speech_run_ = 0;
  active_speech_run_ = start_frames;
  silence_run_ = 0;
  resume_reported_ = false;
  if (on_speech_started_) {
    on_speech_started_();
  }
  if (!resume_reported_ && config_.resume_frames > 0 &&
      active_speech_run_ >= config_.resume_frames) {
    resume_reported_ = true;
    if (on_resume_detected_) {
      on_resume_detected_();
    }
  }
  asr_.set_event_callback([this](const backend::BackendEvent& event) {
    if (event.kind == backend::BackendEvent::Kind::kFinal) {
      if (on_final_ && !event.text.empty()) {
        on_final_(event.text);
      }
    }
  });
  // 起音帧之前保留的前置缓冲全部属于本话语，按顺序先送 ASR。
  for (const auto& frame : pre_roll_) {
    asr_.feed_audio(frame, false);
  }
  pre_roll_.clear();
}

void StreamingInput::end_utterance() {
  if (config_.speech_detector_reset) {
    config_.speech_detector_reset();
  }
  pre_roll_.clear();
  speech_run_ = 0;
  active_speech_run_ = 0;
  silence_run_ = 0;
  resume_reported_ = false;
  active_ = false;
}

}  // namespace slotnexus::session
