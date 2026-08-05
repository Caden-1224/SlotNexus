// 任务运行时：work_id → TaskChannel 的任务表。
//
// 负责：
//   - work_id 分配（复用 TaskRegistry：单调递增、不回收复用）与容量上限；
//   - 未知 work_id 的 kNotExist 语义；
//   - 后端工厂：每次 setup 产出独立后端实例（未来每任务一个模型上下文）。
#pragma once

#include "voxorchestra/runtime/task_channel.hpp"
#include "voxorchestra/task_registry/task_registry.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace voxorchestra::runtime {

class TaskRuntime {
 public:
  // 默认工厂产出 EchoBackend（端到端冒烟与单元测试用）。
  explicit TaskRuntime(std::size_t max_tasks = 0);

  // 自定义后端工厂；max_tasks 为 0 时使用默认容量。
  TaskRuntime(std::function<std::shared_ptr<IBackend>()> backend_factory,
              std::size_t max_tasks = 0);
  ~TaskRuntime();

  TaskRuntime(const TaskRuntime&) = delete;
  TaskRuntime& operator=(const TaskRuntime&) = delete;

  // setup：分配新 work_id 并创建任务通道；容量耗尽返回 kCapacity。
  struct SetupResult {
    TaskChannel::Error error = TaskChannel::Error::kOk;
    std::string work_id;
  };
  SetupResult setup(const std::string& request_id, const std::string& payload);

  // 使用外部指定的 work_id 创建任务（Unit Manager 全局分配，节点侧使用）；
  // 同名任务已存在返回 kBadState，容量耗尽返回 kCapacity。
  SetupResult setup_with(const std::string& work_id, const std::string& request_id,
                         const std::string& payload);

  TaskChannel::Error inference(const std::string& work_id,
                               const std::string& request_id,
                               const std::string& payload,
                               std::chrono::milliseconds timeout,
                               std::string* out_text,
                               const EventSink& events = {});
  TaskChannel::Error cancel(const std::string& work_id, const std::string& request_id);
  TaskChannel::Error exit(const std::string& work_id);

  // 未知任务返回 std::nullopt。
  std::optional<TaskChannel::TaskInfo> taskinfo(const std::string& work_id) const;

  bool is_alive(const std::string& work_id) const;
  std::size_t size() const;

 private:
  // 前置条件：调用方已持有 map_mutex_。
  std::shared_ptr<TaskChannel> find_locked(const std::string& work_id) const;

  std::function<std::shared_ptr<IBackend>()> backend_factory_;
  task_registry::TaskRegistry registry_;  // work_id 分配与容量控制
  mutable std::mutex map_mutex_;
  std::unordered_map<std::string, std::shared_ptr<TaskChannel>> channels_;
};

}  // namespace voxorchestra::runtime
