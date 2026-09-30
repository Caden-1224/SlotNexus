// 唤醒输入确定性测试：WakeGate 状态迁移 + StreamingInput 帧到独立轮次。
// Author: Caden
#include "slotnexus/session/streaming_input.hpp"
#include "slotnexus/session/wake_gate.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using slotnexus::backend::BackendEvent;
using slotnexus::backend::EventCallback;
using slotnexus::backend::IAsrBackend;
using slotnexus::session::StreamingInput;
using slotnexus::session::WakeGate;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      ++g_failures;                                                        \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #cond \
                << std::endl;                                              \
    }                                                                      \
  } while (0)

// 脚本化 ASR：每次 is_last 弹出预先排好的 final 文本，不模拟声学模型。
class ScriptedAsrBackend final : public IAsrBackend {
 public:
  void push_final(std::string text) { finals_.push_back(std::move(text)); }

  void set_event_callback(EventCallback cb) override { cb_ = std::move(cb); }

  void feed_audio(const std::vector<std::int16_t>& /*pcm*/,
                  bool is_last) override {
    if (!cb_ || !is_last) {
      return;
    }
    std::string text = "空";
    if (!finals_.empty()) {
      text = std::move(finals_.front());
      finals_.pop_front();
    }
    cb_({BackendEvent::Kind::kFinal, std::move(text), {}});
  }

  void cancel() override { cb_ = nullptr; }

 private:
  EventCallback cb_;
  std::deque<std::string> finals_;
};

StreamingInput::Config MakeStreamConfig() {
  StreamingInput::Config cfg;
  cfg.frame_samples = 320;
  cfg.pre_roll_frames = 1;
  cfg.min_speech_frames = 1;
  cfg.min_silence_frames = 2;
  cfg.speech_rms_threshold = 250;
  return cfg;
}

WakeGate::Config MakeGateConfig() {
  WakeGate::Config cfg;
  cfg.enabled = true;
  cfg.wake_words = {"小凯"};
  cfg.sleep_words = {"退下"};
  cfg.follow_up_timeout = 5000ms;
  cfg.max_session = 5000ms;
  cfg.max_turns = 6;
  return cfg;
}

// 测试侧直接按 session 采集线程的职责组合 StreamingInput 与 WakeGate：
// KWS 触发只管休眠→唤醒，ASR final 经 WakeGate 接纳为独立轮次。
class WakeInputHarness {
 public:
  WakeInputHarness(ScriptedAsrBackend& asr, WakeGate::Config gate_config)
      : asr_(asr), gate_(std::move(gate_config)) {
    stream_ = std::make_unique<StreamingInput>(MakeStreamConfig(), asr_);
    stream_->set_callbacks(
        [this](std::string text) {
          const auto result = gate_.process(text);
          if (result.answer) {
            turns.push_back(std::move(result.text));
          }
        },
        {}, [this] {}, [this] { gate_.speech_started(); });
  }

  void Feed(const std::vector<std::int16_t>& pcm) {
    if (kws_trigger && gate_.state() == WakeGate::State::kSleeping) {
      kws_trigger = false;
      gate_.wake("小凯");
    }
    stream_->feed_audio(pcm.data(), pcm.size());
    gate_.tick();
  }

  void FeedUtterance() {
    std::vector<std::int16_t> speech(320, 1000);
    std::vector<std::int16_t> silence(320, 0);
    Feed(speech);
    Feed(speech);
    Feed(silence);
    Feed(silence);
  }

  ScriptedAsrBackend& asr_;
  WakeGate gate_;
  std::unique_ptr<StreamingInput> stream_;
  std::deque<std::string> turns;
  bool kws_trigger = false;
};

void TestWakeGateDisabledAnswersAll() {
  WakeGate::Config cfg;
  cfg.enabled = false;
  WakeGate gate(std::move(cfg));
  const auto result = gate.process("你好");
  CHECK(result.answer);
  CHECK(result.text == "你好");
  CHECK(gate.state() == WakeGate::State::kListening);
}

void TestWakeGateSleepWordAndWakeWordMatch() {
  WakeGate gate(MakeGateConfig());
  CHECK(!gate.accepts_wake_word("小乐"));
  CHECK(gate.accepts_wake_word("小凯"));
  CHECK(gate.state() == WakeGate::State::kSleeping);
  CHECK(gate.wake("小凯"));
  CHECK(gate.state() == WakeGate::State::kListening);

  const auto result = gate.process("退下");
  CHECK(result.slept);
  CHECK(!result.answer);
  CHECK(gate.state() == WakeGate::State::kSleeping);
  CHECK(gate.process("休眠期不应回答").answer == false);

  CHECK(gate.wake("小凯"));
  CHECK(gate.process("重新唤醒").answer);
  CHECK(gate.turns() == 1);
}

void TestWakeAndTwoIndependentTurns() {
  ScriptedAsrBackend asr;
  WakeInputHarness input(asr, MakeGateConfig());

  asr.push_final("第一问");
  input.kws_trigger = true;
  input.FeedUtterance();
  CHECK(input.turns.size() == 1);
  CHECK(input.turns.at(0) == "第一问");
  CHECK(input.gate_.state() == WakeGate::State::kProcessing);

  input.gate_.finish_turn();
  CHECK(input.gate_.state() == WakeGate::State::kFollowUp);

  asr.push_final("第二问");
  input.FeedUtterance();
  CHECK(input.turns.size() == 2);
  CHECK(input.turns.at(1) == "第二问");
  CHECK(input.gate_.turns() == 2);
}

void TestSleepWordAndRewake() {
  ScriptedAsrBackend asr;
  WakeInputHarness input(asr, MakeGateConfig());

  asr.push_final("退下");
  input.kws_trigger = true;
  input.FeedUtterance();
  CHECK(input.turns.empty());
  CHECK(input.gate_.state() == WakeGate::State::kSleeping);

  asr.push_final("重新唤醒后的问题");
  input.kws_trigger = true;
  input.FeedUtterance();
  CHECK(input.turns.size() == 1);
  CHECK(input.turns.at(0) == "重新唤醒后的问题");
  CHECK(input.gate_.turns() == 1);
}

void TestFollowUpTimeoutAndMaxTurns() {
  auto gate_cfg = MakeGateConfig();
  gate_cfg.follow_up_timeout = 30ms;
  ScriptedAsrBackend asr;
  WakeInputHarness input(asr, gate_cfg);

  asr.push_final("第一问");
  input.kws_trigger = true;
  input.FeedUtterance();
  CHECK(input.turns.size() == 1);
  input.gate_.finish_turn();

  std::this_thread::sleep_for(60ms);
  input.gate_.tick();
  CHECK(input.gate_.state() == WakeGate::State::kSleeping);

  asr.push_final("休眠期不应回答");
  input.FeedUtterance();
  CHECK(input.turns.size() == 1);

  auto max_cfg = MakeGateConfig();
  max_cfg.max_turns = 1;
  ScriptedAsrBackend max_asr;
  WakeInputHarness max_input(max_asr, max_cfg);
  max_asr.push_final("唯一一轮");
  max_input.kws_trigger = true;
  max_input.FeedUtterance();
  CHECK(max_input.turns.size() == 1);
  max_input.gate_.finish_turn();

  max_asr.push_final("超出轮数");
  max_input.FeedUtterance();
  CHECK(max_input.turns.size() == 1);
  CHECK(max_input.gate_.state() == WakeGate::State::kSleeping);

  max_asr.push_final("重新唤醒后的新一轮");
  max_input.kws_trigger = true;
  max_input.FeedUtterance();
  CHECK(max_input.turns.size() == 2);
  CHECK(max_input.gate_.turns() == 1);
}

}  // namespace

int main() {
  TestWakeGateDisabledAnswersAll();
  TestWakeGateSleepWordAndWakeWordMatch();
  TestWakeAndTwoIndependentTurns();
  TestSleepWordAndRewake();
  TestFollowUpTimeoutAndMaxTurns();

  if (g_failures == 0) {
    std::cout << "wake_input_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "wake_input_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
