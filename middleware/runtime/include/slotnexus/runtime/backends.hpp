// 通用模拟推理 Backend：Echo（立即返回）与 Delay（协作式耗时推理）。
// Author: Caden
//
// 设计意图：为中间件状态机与核心 E2E 测试提供确定性的 IBackend 实现。
// 两者不依赖任何语音类型：输入输出均为 JSON。为兼容既有 echo 演示，
// 当请求对象含字符串字段 text 时，Echo/Delay 返回 {"text":"echo:"+text}；
// 其他请求原样回显。语音模块不走这里，而是使用自己的 JSON 适配器。
#pragma once

#include "slotnexus/runtime/ibackend.hpp"

#include <chrono>
#include <thread>

namespace slotnexus::runtime {

inline nlohmann::json EchoPayload(const nlohmann::json& request) {
  if (request.is_object() && request.contains("text") &&
      request["text"].is_string()) {
    return nlohmann::json{{"text", "echo:" + request["text"].get<std::string>()}};
  }
  return request;
}

// 立即返回模拟结果；忽略超时与取消。
class EchoBackend final : public IBackend {
 public:
  BackendResult infer(const nlohmann::json& request,
                      std::chrono::steady_clock::time_point /*deadline*/,
                      const std::atomic<bool>& /*cancelled*/,
                      const EventSink& /*events*/) override {
    return {BackendResult::Code::kOk, EchoPayload(request)};
  }
};

// 模拟耗时推理：至少耗时 delay_ 才返回，期间每 10ms 协作式检查 deadline 与
// 取消标志并尽快返回。用于测试超时、取消与单流（kBusy）语义。
class DelayBackend final : public IBackend {
 public:
  explicit DelayBackend(std::chrono::milliseconds delay) : delay_(delay) {}

  BackendResult infer(const nlohmann::json& request,
                      std::chrono::steady_clock::time_point deadline,
                      const std::atomic<bool>& cancelled,
                      const EventSink& /*events*/) override {
    constexpr std::chrono::milliseconds kSlice(10);
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
      if (cancelled.load()) {
        return {BackendResult::Code::kCancelled, nlohmann::json::object()};
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return {BackendResult::Code::kTimeout, nlohmann::json::object()};
      }
      if (now - start >= delay_) {
        return {BackendResult::Code::kOk, EchoPayload(request)};
      }
      std::this_thread::sleep_for(kSlice);
    }
  }

 private:
  const std::chrono::milliseconds delay_;
};

}  // namespace slotnexus::runtime
