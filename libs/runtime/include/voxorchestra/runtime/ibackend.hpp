// 推理后端统一接口（Runtime 接口）。
//
// Echo / Delay 为本地模拟实现，用于端到端冒烟与状态机测试；
// 未来 RKLLM / sherpa-onnx / SummerTTS 等硬件后端实现同一接口即可
// 接入节点运行时，任务状态机无需任何改动。
#pragma once

#include <atomic>
#include <chrono>
#include <string>

namespace voxorchestra::runtime {

// 推理结果：code 区分成功 / 超时 / 被取消；text 为产出文本（取消/超时可为空）。
struct BackendResult {
  enum class Code { kOk, kTimeout, kCancelled };

  Code code = Code::kOk;
  std::string text;
};

class IBackend {
 public:
  virtual ~IBackend() = default;

  // 同步推理。deadline 与 cancelled 为协作式中断信号：耗时实现应在循环中
  // 周期检查二者，触发后尽快返回（kTimeout / kCancelled），不要硬等待。
  virtual BackendResult infer(const std::string& payload,
                              std::chrono::steady_clock::time_point deadline,
                              const std::atomic<bool>& cancelled) = 0;
};

}  // namespace voxorchestra::runtime
