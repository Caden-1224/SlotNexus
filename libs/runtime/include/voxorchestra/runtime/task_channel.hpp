// 任务通道：单个任务（work_id）的请求状态机。
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

#include "voxorchestra/runtime/ibackend.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

namespace voxorchestra::runtime {

class TaskChannel {
 public:
  enum class State { kNew, kReady, kBusy, kTerminated };

  enum class Error {
    kOk,         // 操作成功
    kNotExist,   // 任务不存在（exit 后所有操作、未知 work_id）
    kBadState,   // 当前状态不允许该操作（重复 setup、未 setup 先推理）
    kBusy,       // 已有在途请求（单流语义）
    kTimeout,    // 推理超时
    kCancelled,  // 推理被取消
    kCapacity,   // 任务容量耗尽
  };

  // taskinfo 上报的任务快照。
  struct TaskInfo {
    State state = State::kNew;
    std::string work_id;        // 本任务 id（创建方注入）
    std::string in_flight;      // 在途请求的 request_id，空闲时为空串
    std::string setup_payload;  // setup 携带的载荷（如模型配置）
    std::size_t inference_count = 0;  // 已发起的推理次数
  };

  // work_id 由创建方（TaskRuntime）分配后注入。
  explicit TaskChannel(std::string work_id, std::shared_ptr<IBackend> backend);
  ~TaskChannel();

  TaskChannel(const TaskChannel&) = delete;
  TaskChannel& operator=(const TaskChannel&) = delete;

  // kNew → kReady。重复 setup 返回 kBadState。
  Error setup(const std::string& request_id, const std::string& payload);

  // kReady → kBusy → kReady。单流：kBusy 时再次发起返回 kBusy；
  // 未 setup 返回 kBadState。timeout 为 0 时使用默认超时；
  // out_text 非空且成功时回填推理结果。
  // events 为推理过程的事件出口（流式后端中间事件），原样透传后端。
  Error inference(const std::string& request_id, const std::string& payload,
                  std::chrono::milliseconds timeout, std::string* out_text,
                  const EventSink& events = {});

  // 取消在途请求（协作式，后端尽快响应后回到 kReady）；
  // kReady 时为空操作返回 kOk。
  Error cancel(const std::string& request_id);

  // 任意状态 → kTerminated；重复 exit 返回 kNotExist。
  Error exit();

  TaskInfo taskinfo() const;
  State state() const;

 private:
  std::atomic<bool> cancel_flag_{false};
  mutable std::mutex mutex_;
  const std::string work_id_;
  const std::shared_ptr<IBackend> backend_;
  State state_ = State::kNew;
  std::string in_flight_;
  std::string setup_payload_;
  std::size_t inference_count_ = 0;
};

// 默认推理超时：客户端未指定时的兜底值。
constexpr std::chrono::milliseconds kDefaultInferenceTimeout(5000);

const char* to_string(TaskChannel::State state);
const char* to_string(TaskChannel::Error error);

}  // namespace voxorchestra::runtime
