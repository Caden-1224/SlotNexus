// 流式 ASR 的节点侧输入接缝：按 work_id 找到已 setup 的 AsrNodeBackend，
// 由独立数据通道逐帧喂入，而不是每次推理都走整段 RPC 负载。
// Author: Caden
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "slotnexus/backend/backend_event.hpp"

namespace slotnexus::backend {

class IStreamingAsrBackend {
 public:
  using EventSink = std::function<void(const BackendEvent&)>;

  virtual ~IStreamingAsrBackend() = default;

  // 开始一次流式识别；sink 接收 partial/final 事件。
  virtual bool stream_start(const std::string& request_id, EventSink sink) = 0;

  // 喂入一帧；is_last 表示当前话语结束，实现应产出 final。
  virtual bool stream_feed(const std::vector<std::int16_t>& pcm,
                           bool is_last) = 0;

  // 中止当前流，丢弃未完成结果。
  virtual void stream_cancel() = 0;
};

}  // namespace slotnexus::backend
