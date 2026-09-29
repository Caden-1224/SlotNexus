// 任务通道：单个任务（work_id）的请求状态机。
// Author: Caden
//
// 设计意图：把“任务生命周期”和“后端推理”解耦。输入/输出/事件全部经过
// 通用 JSON 契约，状态机不解释应用负载。
//
// 状态流转：
//   kNew --setup--> kReady <--inference/cancel--> kBusy --exit--> kTerminated
//
// 语义保证：
//   - 单流：同一任务同时只允许一个在途推理，再次发起返回 kBusy；
//   - 协作式取消：cancel 置位原子标志，由后端在耗时循环中尽快响应；
//   - exit 后通道进入 kTerminated，一切操作返回 kNotExist（含重复 exit）；
//   - 线程安全：同一任务可被多个线程并发调用（如推理线程 + 管理线程）。
#pragma once

#include "slotnexus/runtime/ibackend.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

namespace slotnexus::runtime {

class TaskChannel {
 public:
  enum class State { kNew, kReady, kBusy, kTerminated };

  enum class Error {
    kOk,
    kNotExist,
    kBadState,
    kBusy,
    kTimeout,
    kCancelled,
    kCapacity,
  };

  // taskinfo 上报的任务快照；setup_payload 为 JSON 的紧凑字符串快照。
  struct TaskInfo {
    State state = State::kNew;
    std::string work_id;
    std::string in_flight;
    std::string setup_payload;
    std::size_t inference_count = 0;
  };

  explicit TaskChannel(std::string work_id, std::shared_ptr<IBackend> backend);
  ~TaskChannel();

  TaskChannel(const TaskChannel&) = delete;
  TaskChannel& operator=(const TaskChannel&) = delete;

  // kNew → kReady。重复 setup 返回 kBadState。
  Error setup(const std::string& request_id, const nlohmann::json& payload);

  // kReady → kBusy → kReady。单流：kBusy 时再次发起返回 kBusy；
  // 未 setup 返回 kBadState。timeout 为 0 时使用默认超时；
  // out 非空且成功时回填推理结果的 JSON 负载。
  Error inference(const std::string& request_id, const nlohmann::json& payload,
                  std::chrono::milliseconds timeout, nlohmann::json* out,
                  const EventSink& events = {});

  // 取消在途请求（协作式，后端尽快响应后回到 kReady）；
  // kReady 时为空操作返回 kOk。
  Error cancel(const std::string& request_id);

  // 任意状态 → kTerminated；重复 exit 返回 kNotExist。
  Error exit();

  TaskInfo taskinfo() const;
  State state() const;

  // 后端实例借用；生命周期由 TaskChannel 持有。仅供同一节点内的流式
  // 输入通道按 work_id 找到已 setup 的后端，不转移所有权。
  std::shared_ptr<IBackend> backend() const { return backend_; }

 private:
  std::atomic<bool> cancel_flag_{false};
  mutable std::mutex mutex_;
  const std::string work_id_;
  const std::shared_ptr<IBackend> backend_;
  State state_ = State::kNew;
  std::string in_flight_;
  nlohmann::json setup_payload_ = nlohmann::json::object();
  std::size_t inference_count_ = 0;
};

// 默认推理超时：客户端未指定时的兜底值。
constexpr std::chrono::milliseconds kDefaultInferenceTimeout(5000);

const char* to_string(TaskChannel::State state);
const char* to_string(TaskChannel::Error error);

}  // namespace slotnexus::runtime
