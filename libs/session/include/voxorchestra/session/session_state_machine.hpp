// Session 状态机：编排阶段的显式状态与合法迁移（可测试、可审计）。
//
// 状态：
//   Idle → Listening → Routing → Thinking → Speaking → Idle
//                            ↘ 任意阶段 --cancel--> Cancelling → Idle
//
// 合法迁移表：
//   Idle      --audio_start--> Listening
//   Listening --asr_final-----> Routing
//   Routing   --route_l0_l1---> Speaking   （直答，绕过 LLM）
//   Routing   --route_l2_l3---> Thinking   （调用 LLM）
//   Thinking  --llm_done------> Speaking
//   Speaking  --tts_done------> Idle
//   Idle / Listening / Routing / Thinking / Speaking --cancel--> Cancelling
//   Cancelling --cancel_complete--> Idle
//   Idle      --cancel--------> Idle（空操作，幂等）
//
// 非法迁移返回 false 且状态不变；每次成功迁移记录轨迹
// （"state--event-->state" 字符串列表），供测试断言与日志审计。
// 线程安全：状态与轨迹在互斥量保护下读写。
#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace voxorchestra::session {

class SessionStateMachine {
 public:
  enum class State { kIdle, kListening, kRouting, kThinking, kSpeaking,
                     kCancelling };

  enum class Event {
    kAudioStart,     // 开始输入音频（WAV/麦克风）
    kAsrFinal,       // ASR 产出最终文本
    kRouteL0L1,      // 路由到直答（L0/L1）
    kRouteL2L3,      // 路由到 LLM（L2/L3）
    kLlmDone,        // LLM 生成结束
    kTtsDone,        // TTS 合成与写出结束
    kCancel,         // 收到取消请求
    kCancelComplete, // 取消清理完成，回到 Idle
  };

  // 提交一个事件；合法返回 true 并迁移，非法返回 false 且状态不变。
  bool dispatch(Event event);

  State state() const;
  const char* state_name() const;  // "idle"/"listening"/... （日志与 taskinfo）

  // 迁移轨迹（只读快照），供测试断言与证据记录。
  std::vector<std::string> trace() const;

  // 回到 Idle（不记录为迁移），清空轨迹；重复 reset 幂等。
  void reset();

 private:
  State state_ = State::kIdle;
  mutable std::mutex mutex_;
  std::vector<std::string> trace_;
};

}  // namespace voxorchestra::session
