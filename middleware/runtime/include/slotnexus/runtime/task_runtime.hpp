// 任务运行时：work_id → TaskChannel 的任务表。
// Author: Caden
//
// 设计意图：把全局任务标识（TaskRegistry）与单任务状态机组合在一起，
// 对上层只暴露通用 JSON 输入/输出和事件出口，不绑定任何应用模块。
//
// 主要逻辑：
//   - work_id 分配与容量控制；
//   - setup/setup_with 创建独立后端实例（工厂注入）；
//   - inference/cancel/exit/taskinfo 按 work_id 路由。
#pragma once

#include "slotnexus/runtime/task_channel.hpp"
#include "slotnexus/task_registry/task_registry.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace slotnexus::runtime {

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

  struct SetupResult {
    TaskChannel::Error error = TaskChannel::Error::kOk;
    std::string work_id;
  };
  SetupResult setup(const std::string& request_id,
                    const nlohmann::json& payload);

  // 使用外部指定的 work_id 创建任务（Manager 全局分配，节点侧使用）。
  SetupResult setup_with(const std::string& work_id,
                         const std::string& request_id,
                         const nlohmann::json& payload);

  TaskChannel::Error inference(const std::string& work_id,
                               const std::string& request_id,
                               const nlohmann::json& payload,
                               std::chrono::milliseconds timeout,
                               nlohmann::json* out,
                               const EventSink& events = {});
  TaskChannel::Error cancel(const std::string& work_id,
                            const std::string& request_id);
  TaskChannel::Error exit(const std::string& work_id);

  std::optional<TaskChannel::TaskInfo> taskinfo(
      const std::string& work_id) const;

  bool is_alive(const std::string& work_id) const;
  std::size_t size() const;

 private:
  std::shared_ptr<TaskChannel> find_locked(const std::string& work_id) const;

  std::function<std::shared_ptr<IBackend>()> backend_factory_;
  task_registry::TaskRegistry registry_;
  mutable std::mutex map_mutex_;
  std::unordered_map<std::string, std::shared_ptr<TaskChannel>> channels_;
};

}  // namespace slotnexus::runtime
