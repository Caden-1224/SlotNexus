// 推理后端统一接口（Runtime 接口）。
//
// Echo / Delay 为本地模拟实现，用于端到端冒烟与状态机测试；
// 未来 RKLLM / sherpa-onnx / SummerTTS 等硬件后端实现同一接口即可
// 接入节点运行时，任务状态机无需任何改动。
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>

#include "voxorchestra/backend/backend_event.hpp"

namespace voxorchestra::runtime {

// 推理结果：code 区分成功 / 超时 / 被取消；text 为产出文本（取消/超时可为空）。
struct BackendResult {
  enum class Code { kOk, kTimeout, kCancelled };

  Code code = Code::kOk;
  std::string text;
};

// 事件出口：流式后端（ASR/LLM/TTS）在推理过程中产生的中间事件
// （partial/token/PCM 帧等）经此回调实时转发。同步后端（Echo/Delay/RAG）
// 不产生事件，回调为空或忽略即可。标识（work_id/request_id）不属于
// 后端事件，由节点外壳在转发时关联（见 dataplane 的 DataplaneEvent）。
using EventSink = std::function<void(const backend::BackendEvent&)>;

class IBackend {
 public:
  virtual ~IBackend() = default;

  // 同步推理。deadline 与 cancelled 为协作式中断信号：耗时实现应在循环中
  // 周期检查二者，触发后尽快返回（kTimeout / kCancelled），不要硬等待。
  // events 为可选事件出口：产生中间事件的后端在驱动过程中调用（可空，
  // 空回调直接忽略），结束事件（kDone/kFinal）也经其透出。
  virtual BackendResult infer(const std::string& payload,
                              std::chrono::steady_clock::time_point deadline,
                              const std::atomic<bool>& cancelled,
                              const EventSink& events) = 0;
};

}  // namespace voxorchestra::runtime
