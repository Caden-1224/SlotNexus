// 语音合成后端契约（流式）。
// Author: Caden
//
// 用法：synthesize(text) 产出 kPcm…（每帧一事件），全部音频产出后 kDone。
// 取消后实现不得再产出事件。
//
// 可替换实现：FakeTtsBackend（默认） / SummerTtsBackend（板端，）。
#pragma once

#include <string>

#include "slotnexus/backend/backend_event.hpp"

namespace slotnexus::backend {

class ITtsBackend {
public:
 virtual ~ITtsBackend() = default;

 // 设置事件回调；每次 synthesize 前调用（实现应清空会话内状态）。
 virtual void set_event_callback(EventCallback cb) = 0;

 // 合成：以文本为输入，经回调产出 PCM 帧事件流，结束时产出 kDone。
 virtual void synthesize(const std::string& text) = 0;

 // 协作式取消：置位后不再产出任何事件，synthesize 变为空操作。
 virtual void cancel() = 0;
};

}  // namespace slotnexus::backend
