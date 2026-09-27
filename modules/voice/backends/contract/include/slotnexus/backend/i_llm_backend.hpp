// 大模型文本生成后端契约（流式）。
// Author: Caden
//
// 用法：generate(prompt) 产出 kToken…（每 token 一条事件），全部 token 结束后
// 产出 kDone。取消后实现不得再产出事件。
//
// 可替换实现：FakeLlmBackend（默认） / RkllmBackend（板端，）。
#pragma once

#include <string>

#include "slotnexus/backend/backend_event.hpp"

namespace slotnexus::backend {

class ILlmBackend {
public:
 virtual ~ILlmBackend() = default;

 // 设置事件回调；每次 generate 前调用（实现应清空会话内状态）。
 virtual void set_event_callback(EventCallback cb) = 0;

 // 生成：以 prompt 为输入，经回调产出 token 事件流，结束时产出 kDone。
 virtual void generate(const std::string& prompt) = 0;

 // 协作式取消：置位后不再产出任何事件，generate 变为空操作。
 virtual void cancel() = 0;
};

}  // namespace slotnexus::backend
