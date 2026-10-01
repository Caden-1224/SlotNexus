// ContinuousGate：任务会话内的连续交互状态机。
// Author: Caden
//
// 职责：维护“sleeping → listening → 独立轮次 → sleeping”的状态、轮数和
// 截止时间，不访问 ASR/声卡/管线。显式启动入口调用 start()；ASR final
// 文本通过 process() 接纳为轮次或触发休眠；一轮处理结束后由调用方
// finish_turn() 进入跟进空闲。所有公开方法线程安全。
#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace slotnexus::session {

class ContinuousGate {
 public:
  enum class State { kSleeping, kListening, kProcessing, kFollowUp };

  struct Config {
    bool enabled = false;
    std::vector<std::string> sleep_words;
    std::chrono::milliseconds follow_up_timeout{60000};
    std::chrono::milliseconds max_session{300000};
    int max_turns = 6;
  };

  struct Result {
    bool answer = false;
    std::string text;
    bool slept = false;
    bool expired = false;
  };

  ContinuousGate() = default;
  explicit ContinuousGate(Config config) { configure(std::move(config)); }

  void configure(Config config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = std::move(config);
    state_ = State::kSleeping;
    turns_ = 0;
    session_started_ = {};
    follow_up_deadline_ = {};
  }

  // 由显式启动入口触发。仅在启用且 Sleeping 时进入 Listening，返回是否发生迁移。
  bool start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.enabled) {
      return false;
    }
    const auto now = Clock::now();
    expire_locked(now);
    if (state_ != State::kSleeping) {
      return false;
    }
    state_ = State::kListening;
    session_started_ = now;
    turns_ = 0;
    return true;
  }

  // ASR final 到轮次调度的入口。answer=true 时 text 是本轮有效输入。
  // continuation=true 表示这是观察窗内检测到续说后的合并 final：
  // 它属于上一轮，不应再次增加 turns_。
  Result process(const std::string& asr_text, bool continuation = false) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string text = normalize(asr_text);
    if (!config_.enabled) {
      return {true, text, false, false};
    }

    const auto now = Clock::now();
    expire_locked(now);
    if (state_ == State::kSleeping) {
      return {};
    }
    if (contains_any(text, config_.sleep_words)) {
      sleep_locked();
      return {false, {}, true, false};
    }
    if (text.empty()) {
      return {};
    }
    if (continuation) {
      // 续说 final 仍属于当前轮；只要会话没有结束，就允许把状态拉回
      // Processing 并返回合并文本，但不重复增加 turns_。
      if (state_ == State::kListening || state_ == State::kFollowUp ||
          state_ == State::kProcessing) {
        state_ = State::kProcessing;
        return {true, text, false, false};
      }
      return {};
    }
    if (config_.max_turns > 0 && turns_ >= config_.max_turns) {
      sleep_locked();
      return {false, {}, false, true};
    }
    if (state_ == State::kProcessing) {
      // 新一轮起音边沿应先调用 speech_started() 把 Processing 切回
      // Listening；未观察到起音边沿的重复 final 不重复开轮。
      return {};
    }

    state_ = State::kProcessing;
    ++turns_;
    return {true, text, false, false};
  }

  // VAD 起音边沿：FollowUp/Processing 回到 Listening，可接纳下一轮。
  void speech_started() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.enabled) {
      return;
    }
    expire_locked(Clock::now());
    if (state_ == State::kFollowUp || state_ == State::kProcessing) {
      state_ = State::kListening;
    }
  }

  // 一轮管线处理结束：进入 FollowUp，等待跟进空闲超时。
  void finish_turn() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.enabled || state_ != State::kProcessing) {
      return;
    }
    state_ = State::kFollowUp;
    follow_up_deadline_ = Clock::now() + config_.follow_up_timeout;
  }

  // 检查空闲与总时长；返回本次调用是否发生状态迁移。
  bool tick() {
    std::lock_guard<std::mutex> lock(mutex_);
    const State before = state_;
    expire_locked(Clock::now());
    return before != state_;
  }

  State state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const_cast<ContinuousGate*>(this)->expire_locked(Clock::now());
    return state_;
  }

  const char* state_name() const {
    switch (state()) {
      case State::kSleeping:
        return "sleeping";
      case State::kListening:
        return "listening";
      case State::kProcessing:
        return "processing";
      case State::kFollowUp:
        return "follow_up";
    }
    return "unknown";
  }

  bool enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.enabled;
  }

  int turns() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return turns_;
  }

 private:
  using Clock = std::chrono::steady_clock;

  void expire_locked(Clock::time_point now) {
    if (!config_.enabled || state_ == State::kSleeping) {
      return;
    }
    if (config_.max_session.count() > 0 &&
        now - session_started_ >= config_.max_session) {
      sleep_locked();
      return;
    }
    if (state_ == State::kFollowUp && config_.follow_up_timeout.count() > 0 &&
        now >= follow_up_deadline_) {
      sleep_locked();
    }
  }

  void sleep_locked() {
    state_ = State::kSleeping;
    turns_ = 0;
    follow_up_deadline_ = {};
  }

  static std::string normalize(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
      if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
        continue;
      }
      out.push_back(ch);
    }
    return out;
  }

  static bool contains_any(const std::string& text,
                           const std::vector<std::string>& words) {
    for (const auto& word : words) {
      if (!word.empty() && text.find(word) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  Config config_;
  State state_ = State::kSleeping;
  int turns_ = 0;
  Clock::time_point session_started_{};
  Clock::time_point follow_up_deadline_{};
  mutable std::mutex mutex_;
};

}  // namespace slotnexus::session
