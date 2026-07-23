// Session 状态机单元测试：合法迁移、非法迁移拒绝、任意阶段取消、轨迹记录。
#include "voxorchestra/session/session_state_machine.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace ss = voxorchestra::session;
using ss::SessionStateMachine;

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

// 完整 L0/L1 链路：Idle→Listening→Routing→Speaking→Idle。
void test_happy_path_l0l1() {
  SessionStateMachine sm;
  CHECK(sm.state() == SessionStateMachine::State::kIdle);
  CHECK(sm.dispatch(SessionStateMachine::Event::kAudioStart));
  CHECK(sm.state() == SessionStateMachine::State::kListening);
  CHECK(sm.dispatch(SessionStateMachine::Event::kAsrFinal));
  CHECK(sm.state() == SessionStateMachine::State::kRouting);
  CHECK(sm.dispatch(SessionStateMachine::Event::kRouteL0L1));
  CHECK(sm.state() == SessionStateMachine::State::kSpeaking);
  CHECK(sm.dispatch(SessionStateMachine::Event::kTtsDone));
  CHECK(sm.state() == SessionStateMachine::State::kIdle);

  const auto trace = sm.trace();
  CHECK(trace.size() == 4);
  CHECK(trace[0] == "idle--audio_start-->listening");
  CHECK(trace[3] == "speaking--tts_done-->idle");
  std::cout << "  [ok] L0/L1 链路：Idle→Listening→Routing→Speaking→Idle，轨迹正确"
            << std::endl;
}

// L2/L3 链路：路由到 Thinking 后经 LLM 完成进入 Speaking。
void test_happy_path_l2l3() {
  SessionStateMachine sm;
  CHECK(sm.dispatch(SessionStateMachine::Event::kAudioStart));
  CHECK(sm.dispatch(SessionStateMachine::Event::kAsrFinal));
  CHECK(sm.dispatch(SessionStateMachine::Event::kRouteL2L3));
  CHECK(sm.state() == SessionStateMachine::State::kThinking);
  CHECK(sm.dispatch(SessionStateMachine::Event::kLlmDone));
  CHECK(sm.state() == SessionStateMachine::State::kSpeaking);
  CHECK(sm.dispatch(SessionStateMachine::Event::kTtsDone));
  CHECK(sm.state() == SessionStateMachine::State::kIdle);
  std::cout << "  [ok] L2/L3 链路：Routing→Thinking→Speaking→Idle" << std::endl;
}

// 非法迁移：状态不变、返回 false。
void test_illegal_transitions() {
  SessionStateMachine sm;
  // Idle 不能直接进入 Routing/Thinking/Speaking。
  CHECK(!sm.dispatch(SessionStateMachine::Event::kAsrFinal));
  CHECK(!sm.dispatch(SessionStateMachine::Event::kRouteL2L3));
  CHECK(!sm.dispatch(SessionStateMachine::Event::kTtsDone));
  CHECK(sm.state() == SessionStateMachine::State::kIdle);
  // Listening 不能跳过路由直接进入 Speaking。
  sm.dispatch(SessionStateMachine::Event::kAudioStart);
  CHECK(!sm.dispatch(SessionStateMachine::Event::kRouteL0L1));
  CHECK(sm.state() == SessionStateMachine::State::kListening);
  // Thinking 中不能回到 Listening、不能直接直答、不能直接 tts_done。
  sm.dispatch(SessionStateMachine::Event::kAsrFinal);
  sm.dispatch(SessionStateMachine::Event::kRouteL2L3);
  CHECK(!sm.dispatch(SessionStateMachine::Event::kAsrFinal));
  CHECK(!sm.dispatch(SessionStateMachine::Event::kRouteL0L1));
  CHECK(!sm.dispatch(SessionStateMachine::Event::kTtsDone));
  CHECK(sm.state() == SessionStateMachine::State::kThinking);
  std::cout << "  [ok] 非法迁移被拒绝且状态不变" << std::endl;
}

// 任意活动阶段取消：→ Cancelling → Idle。
void test_cancel_from_every_stage() {
  struct Stage {
    const char* name;
    void (*reach)(SessionStateMachine&);
  };
  const Stage stages[] = {
      {"listening", [](SessionStateMachine& s) { s.dispatch(SessionStateMachine::Event::kAudioStart); }},
      {"routing", [](SessionStateMachine& s) {
         s.dispatch(SessionStateMachine::Event::kAudioStart);
         s.dispatch(SessionStateMachine::Event::kAsrFinal);
       }},
      {"thinking", [](SessionStateMachine& s) {
         s.dispatch(SessionStateMachine::Event::kAudioStart);
         s.dispatch(SessionStateMachine::Event::kAsrFinal);
         s.dispatch(SessionStateMachine::Event::kRouteL2L3);
       }},
      {"speaking", [](SessionStateMachine& s) {
         s.dispatch(SessionStateMachine::Event::kAudioStart);
         s.dispatch(SessionStateMachine::Event::kAsrFinal);
         s.dispatch(SessionStateMachine::Event::kRouteL0L1);
       }},
  };
  for (const auto& st : stages) {
    SessionStateMachine sm;
    st.reach(sm);
    CHECK(sm.dispatch(SessionStateMachine::Event::kCancel));
    CHECK(sm.state() == SessionStateMachine::State::kCancelling);
    CHECK(sm.dispatch(SessionStateMachine::Event::kCancelComplete));
    CHECK(sm.state() == SessionStateMachine::State::kIdle);
  }
  // Idle 收到取消：空操作且保持 Idle。
  SessionStateMachine idle_sm;
  CHECK(idle_sm.dispatch(SessionStateMachine::Event::kCancel));
  CHECK(idle_sm.state() == SessionStateMachine::State::kIdle);
  // Cancelling 中重复 cancel 非法；cancel_complete 之后 Idle 再 cancel 为空操作。
  SessionStateMachine sm;
  sm.dispatch(SessionStateMachine::Event::kAudioStart);
  sm.dispatch(SessionStateMachine::Event::kCancel);
  CHECK(!sm.dispatch(SessionStateMachine::Event::kCancel));
  std::cout << "  [ok] 任意阶段取消：→Cancelling→Idle；Idle 取消为空操作"
            << std::endl;
}

// 轨迹记录与 reset。
void test_trace_and_reset() {
  SessionStateMachine sm;
  sm.dispatch(SessionStateMachine::Event::kAudioStart);
  sm.dispatch(SessionStateMachine::Event::kCancel);
  sm.dispatch(SessionStateMachine::Event::kCancelComplete);
  const auto trace = sm.trace();
  CHECK(trace.size() == 3);
  CHECK(trace[0] == "idle--audio_start-->listening");
  CHECK(trace[1] == "listening--cancel-->cancelling");
  CHECK(trace[2] == "cancelling--cancel_complete-->idle");

  sm.reset();
  CHECK(sm.state() == SessionStateMachine::State::kIdle);
  CHECK(sm.trace().empty());
  sm.reset();  // 重复 reset 幂等
  CHECK(sm.state() == SessionStateMachine::State::kIdle);
  std::cout << "  [ok] 轨迹逐条记录，reset 幂等" << std::endl;
}

}  // namespace

int main() {
  std::cout << "session_state_test:" << std::endl;
  test_happy_path_l0l1();
  test_happy_path_l2l3();
  test_illegal_transitions();
  test_cancel_from_every_stage();
  test_trace_and_reset();

  if (g_failures == 0) {
    std::cout << "session_state_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "session_state_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
