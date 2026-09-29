// StreamingInput 单元测试：可控帧下的 VAD 分段、前置缓冲与收尾。
// Author: Caden
#include "slotnexus/session/streaming_input.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

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

std::vector<int16_t> make_frame(int16_t value, std::size_t samples = 320) {
  return std::vector<int16_t>(samples, value);
}

class RecordingAsr final : public slotnexus::backend::IAsrBackend {
 public:
  void set_event_callback(slotnexus::backend::EventCallback cb) override {
    cb_ = std::move(cb);
    non_last_frames_ = 0;
  }

  void feed_audio(const std::vector<int16_t>& /*pcm*/,
                  bool is_last) override {
    if (!is_last) {
      ++non_last_frames_;
      return;
    }
    utterance_frames_.push_back(non_last_frames_);
    const std::string text = "F" + std::to_string(non_last_frames_);
    non_last_frames_ = 0;
    if (cb_) {
      cb_({slotnexus::backend::BackendEvent::Kind::kFinal, text, {}});
    }
  }

  void cancel() override {}

  std::vector<std::size_t> utterance_frames_;
  std::size_t non_last_frames_ = 0;

 private:
  slotnexus::backend::EventCallback cb_;
};

void test_short_pause_stays_one_utterance() {
  RecordingAsr asr;
  slotnexus::session::StreamingInput::Config config;
  config.pre_roll_frames = 2;
  config.min_speech_frames = 1;
  config.min_silence_frames = 3;
  slotnexus::session::StreamingInput input(config, asr);
  std::vector<std::string> finals;
  input.set_callbacks([&](std::string text) { finals.push_back(std::move(text)); });

  input.feed_audio(make_frame(0).data(), 320);
  for (int i = 0; i < 5; ++i) {
    input.feed_audio(make_frame(1000).data(), 320);
  }
  input.feed_audio(make_frame(0).data(), 320);
  for (int i = 0; i < 5; ++i) {
    input.feed_audio(make_frame(1000).data(), 320);
  }
  for (int i = 0; i < 3; ++i) {
    input.feed_audio(make_frame(0).data(), 320);
  }

  CHECK(finals.size() == 1);
  CHECK(asr.utterance_frames_.size() == 1);
  CHECK(asr.utterance_frames_[0] == 14);
  std::cout << "  [ok] 短停顿：同一话语不拆分，前置缓冲不丢" << std::endl;
}

void test_long_pause_splits_into_two_utterances() {
  RecordingAsr asr;
  slotnexus::session::StreamingInput::Config config;
  config.pre_roll_frames = 1;
  config.min_speech_frames = 1;
  config.min_silence_frames = 3;
  slotnexus::session::StreamingInput input(config, asr);
  std::vector<std::string> finals;
  input.set_callbacks([&](std::string text) { finals.push_back(std::move(text)); });

  for (int i = 0; i < 2; ++i) {
    input.feed_audio(make_frame(1000).data(), 320);
  }
  for (int i = 0; i < 3; ++i) {
    input.feed_audio(make_frame(0).data(), 320);
  }
  for (int i = 0; i < 2; ++i) {
    input.feed_audio(make_frame(1000).data(), 320);
  }
  for (int i = 0; i < 3; ++i) {
    input.feed_audio(make_frame(0).data(), 320);
  }

  CHECK(finals.size() == 2);
  CHECK(asr.utterance_frames_.size() == 2);
  CHECK(asr.utterance_frames_[0] == 4);
  CHECK(asr.utterance_frames_[1] == 4);
  std::cout << "  [ok] 长静音：两段独立话语分别收尾" << std::endl;
}

void test_flush_ends_active_utterance() {
  RecordingAsr asr;
  slotnexus::session::StreamingInput::Config config;
  config.pre_roll_frames = 3;
  config.min_speech_frames = 1;
  config.min_silence_frames = 10;
  slotnexus::session::StreamingInput input(config, asr);
  std::vector<std::string> finals;
  input.set_callbacks([&](std::string text) { finals.push_back(std::move(text)); });

  input.feed_audio(make_frame(1000).data(), 320);
  input.feed_audio(make_frame(1000).data(), 320);
  input.flush();

  CHECK(finals.size() == 1);
  CHECK(finals[0] == "F2");
  std::cout << "  [ok] 收尾：输入结束时未等到静音也产生 final" << std::endl;
}

void test_pre_roll_keeps_speech_start() {
  RecordingAsr asr;
  slotnexus::session::StreamingInput::Config config;
  config.pre_roll_frames = 4;
  config.min_speech_frames = 1;
  config.min_silence_frames = 3;
  slotnexus::session::StreamingInput input(config, asr);
  std::vector<std::string> finals;
  input.set_callbacks([&](std::string text) { finals.push_back(std::move(text)); });

  for (int i = 0; i < 4; ++i) {
    input.feed_audio(make_frame(0).data(), 320);
  }
  for (int i = 0; i < 2; ++i) {
    input.feed_audio(make_frame(1000).data(), 320);
  }
  for (int i = 0; i < 3; ++i) {
    input.feed_audio(make_frame(0).data(), 320);
  }

  CHECK(finals.size() == 1);
  CHECK(asr.utterance_frames_.size() == 1);
  CHECK(asr.utterance_frames_[0] == 7);  // 4 帧前置 + 2 帧人声 + 2 帧普通静音（末帧为 is_last）
  std::cout << "  [ok] 前置缓冲：起音前保留帧仍进入 ASR" << std::endl;
}

}  // namespace

int main() {
  std::cout << "streaming_input_test:" << std::endl;
  test_short_pause_stays_one_utterance();
  test_long_pause_splits_into_two_utterances();
  test_flush_ends_active_utterance();
  test_pre_roll_keeps_speech_start();

  if (g_failures == 0) {
    std::cout << "streaming_input_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "streaming_input_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
