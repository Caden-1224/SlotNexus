// 语音识别后端契约（流式）。
// Author: Caden
//
// 用法：一次识别会话 =
//  set_event_callback(cb) → feed_audio(帧) × N → feed_audio(最后一帧, is_last=true)
// 实现按帧产出 kPartial（中间结果可更新），最后一帧后产出 kFinal。
// 取消后实现不得再调用回调。
//
// 可替换实现：FakeAsrBackend（默认） / SherpaAsrBackend（板端，）。
// 接口只依赖 BackendEvent，不含任何厂商 SDK 类型。
#pragma once

#include <cstdint>
#include <vector>

#include "slotnexus/backend/backend_event.hpp"

namespace slotnexus::backend {

class IAsrBackend {
public:
 virtual ~IAsrBackend() = default;

 // 设置事件回调；每次识别会话开始前调用（实现应清空会话内状态）。
 virtual void set_event_callback(EventCallback cb) = 0;

 // 推送一帧 16-bit 单声道音频；is_last 为 true 表示会话最后一帧，
 // 实现应在处理后产出 kFinal 并结束会话。
 virtual void feed_audio(const std::vector<int16_t>& pcm, bool is_last) = 0;

 // 协作式取消：置位后不再产出任何事件，feed_audio 变为空操作。
 virtual void cancel() = 0;
};

}  // namespace slotnexus::backend
