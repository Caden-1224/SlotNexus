// 模拟推理 Backend：Echo（立即返回）与 Delay（协作式耗时推理）。
//
// 两者是头文件内联的轻量模拟实现，与未来硬件后端走同一 IBackend 接口，
// 供端到端流程与任务状态机测试使用。
#pragma once

#include "voxorchestra/runtime/ibackend.hpp"

#include <chrono>
#include <thread>

namespace voxorchestra::runtime {

// 立即返回 "echo:" + payload；忽略超时与取消（模拟瞬时完成的算子）。
class EchoBackend final : public IBackend {
 public:
  BackendResult infer(const std::string& payload,
                      std::chrono::steady_clock::time_point /*deadline*/,
                      const std::atomic<bool>& /*cancelled*/,
                      const EventSink& /*events*/) override {
    return {BackendResult::Code::kOk, "echo:" + payload};
  }
};

// 模拟耗时推理：至少耗时 delay_ 才返回，期间每 10ms 协作式检查 deadline 与
// 取消标志并尽快返回。用于测试超时、取消与单流（kBusy）语义。
class DelayBackend final : public IBackend {
 public:
  explicit DelayBackend(std::chrono::milliseconds delay) : delay_(delay) {}

  BackendResult infer(const std::string& payload,
                      std::chrono::steady_clock::time_point deadline,
                      const std::atomic<bool>& cancelled,
                      const EventSink& /*events*/) override {
    constexpr std::chrono::milliseconds kSlice(10);
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
      if (cancelled.load()) {
        return {BackendResult::Code::kCancelled, {}};
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return {BackendResult::Code::kTimeout, {}};
      }
      if (now - start >= delay_) {
        return {BackendResult::Code::kOk, "echo:" + payload};
      }
      std::this_thread::sleep_for(kSlice);
    }
  }

 private:
  const std::chrono::milliseconds delay_;
};

}  // namespace voxorchestra::runtime
