// 通用推理后端契约：任务输入/输出与中间事件均为不透明 JSON 负载。
// Author: Caden
//
// 设计意图：中间件只负责调度、超时、取消和事件转发，不解释负载中的
// 文本、音频或模型字段。语音节点在各自模块适配器中解析应用协议，并把
// 结果/事件转换为这里的最小通用结构。
//
// 主要结构：
//   - BackendResult：Code + 不透明 JSON payload；
//   - BackendEvent：应用自定义 type + JSON payload + 可选 finish 标记；
//   - EventSink：后端向节点外壳投递通用事件的回调；
//   - IBackend：一次推理的同步驱动入口。
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace slotnexus::runtime {

// 推理结果：code 区分成功/超时/取消；payload 由具体模块定义。
struct BackendResult {
  enum class Code { kOk, kTimeout, kCancelled };

  Code code = Code::kOk;
  nlohmann::json payload = nlohmann::json::object();
};

// 通用流式事件：type 是模块自定义字符串，payload 是不透明 JSON。
// 中间件只按 type 原样转发到数据面，不识别 partial/token/pcm 等具体语义。
struct BackendEvent {
  std::string type;
  nlohmann::json payload = nlohmann::json::object();
  bool finish = false;
};

// 事件出口：后端在推理过程中产生的中间事件经此回调实时转发。
using EventSink = std::function<void(const BackendEvent&)>;

class IBackend {
 public:
  virtual ~IBackend() = default;

  // 同步推理。deadline 与 cancelled 为协作式中断信号：耗时实现应在循环中
  // 周期检查二者，触发后尽快返回 kTimeout / kCancelled。
  // request 为不透明 JSON 负载；事件可选，空回调直接忽略。
  virtual BackendResult infer(const nlohmann::json& request,
                              std::chrono::steady_clock::time_point deadline,
                              const std::atomic<bool>& cancelled,
                              const EventSink& events) = 0;
};

}  // namespace slotnexus::runtime
