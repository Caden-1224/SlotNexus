#include "voxorchestra/session/session_state_machine.hpp"

#include <mutex>
#include <utility>

namespace voxorchestra::session {

namespace {

const char* state_to_string(SessionStateMachine::State s) {
  switch (s) {
    case SessionStateMachine::State::kIdle:        return "idle";
    case SessionStateMachine::State::kListening:   return "listening";
    case SessionStateMachine::State::kRouting:     return "routing";
    case SessionStateMachine::State::kThinking:    return "thinking";
    case SessionStateMachine::State::kSpeaking:    return "speaking";
    case SessionStateMachine::State::kCancelling:  return "cancelling";
  }
  return "unknown";
}

const char* event_to_string(SessionStateMachine::Event e) {
  switch (e) {
    case SessionStateMachine::Event::kAudioStart:     return "audio_start";
    case SessionStateMachine::Event::kAsrFinal:       return "asr_final";
    case SessionStateMachine::Event::kRouteL0L1:      return "route_l0_l1";
    case SessionStateMachine::Event::kRouteL2L3:      return "route_l2_l3";
    case SessionStateMachine::Event::kLlmDone:        return "llm_done";
    case SessionStateMachine::Event::kTtsDone:        return "tts_done";
    case SessionStateMachine::Event::kCancel:         return "cancel";
    case SessionStateMachine::Event::kCancelComplete: return "cancel_complete";
  }
  return "unknown";
}

}  // namespace

bool SessionStateMachine::dispatch(Event event) {
  std::lock_guard<std::mutex> lock(mutex_);
  State next = State::kIdle;
  switch (state_) {
    case State::kIdle:
      if (event == Event::kAudioStart) {
        next = State::kListening;
      } else if (event == Event::kCancel) {
        return true;  // Idle 收到取消：空操作，状态不变
      } else {
        return false;
      }
      break;
    case State::kListening:
      if (event == Event::kAsrFinal) {
        next = State::kRouting;
      } else if (event == Event::kCancel) {
        next = State::kCancelling;
      } else {
        return false;
      }
      break;
    case State::kRouting:
      if (event == Event::kRouteL0L1) {
        next = State::kSpeaking;
      } else if (event == Event::kRouteL2L3) {
        next = State::kThinking;
      } else if (event == Event::kCancel) {
        next = State::kCancelling;
      } else {
        return false;
      }
      break;
    case State::kThinking:
      if (event == Event::kLlmDone) {
        next = State::kSpeaking;
      } else if (event == Event::kCancel) {
        next = State::kCancelling;
      } else {
        return false;
      }
      break;
    case State::kSpeaking:
      if (event == Event::kTtsDone) {
        next = State::kIdle;
      } else if (event == Event::kCancel) {
        next = State::kCancelling;
      } else {
        return false;
      }
      break;
    case State::kCancelling:
      if (event == Event::kCancelComplete) {
        next = State::kIdle;
      } else {
        return false;
      }
      break;
  }

  const char* from = state_to_string(state_);
  const char* to = state_to_string(next);
  trace_.push_back(std::string(from) + "--" + event_to_string(event) + "-->" + to);
  state_ = next;
  return true;
}

SessionStateMachine::State SessionStateMachine::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

const char* SessionStateMachine::state_name() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_to_string(state_);
}

std::vector<std::string> SessionStateMachine::trace() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return trace_;
}

void SessionStateMachine::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = State::kIdle;
  trace_.clear();
}

}  // namespace voxorchestra::session
