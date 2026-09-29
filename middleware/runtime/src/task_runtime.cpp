// 任务运行时实现。
//
// Author: Caden
// 入口：TaskRuntime::setup/setup_with/inference/cancel/exit/taskinfo。
// 关键边界：map_mutex_ 只保护任务表；inference 找到通道后释放锁，避免
// 长时间后端推理阻塞其他 work_id 的 setup/taskinfo。
#include "slotnexus/runtime/task_runtime.hpp"

#include "slotnexus/common/log.hpp"
#include "slotnexus/runtime/backends.hpp"
#include "slotnexus/task_registry/task_registry.hpp"

#include <chrono>
#include <utility>

namespace slotnexus::runtime {

namespace {
std::shared_ptr<IBackend> DefaultBackendFactory() {
  return std::make_shared<EchoBackend>();
}
}  // namespace

TaskRuntime::TaskRuntime(std::size_t max_tasks)
    : TaskRuntime(DefaultBackendFactory, max_tasks) {}

TaskRuntime::TaskRuntime(
    std::function<std::shared_ptr<IBackend>()> backend_factory,
    std::size_t max_tasks)
    : backend_factory_(std::move(backend_factory)), registry_(max_tasks) {}

TaskRuntime::~TaskRuntime() = default;

TaskRuntime::SetupResult TaskRuntime::setup(const std::string& request_id,
                                            const nlohmann::json& payload) {
  SetupResult result;
  std::lock_guard<std::mutex> lock(map_mutex_);
  const std::string work_id = registry_.allocate();
  if (work_id.empty()) {
    result.error = TaskChannel::Error::kCapacity;
    return result;
  }
  const auto setup_start = std::chrono::steady_clock::now();
  auto channel = std::make_shared<TaskChannel>(work_id, backend_factory_());
  const auto backend_init_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - setup_start)
          .count();
  result.error = channel->setup(request_id, payload);
  const auto total_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - setup_start)
          .count();
  common::LogLine("runtime setup work_id=" + work_id + " backend_init_ms=" +
                  std::to_string(backend_init_ms) + " task_setup_ms=" +
                  std::to_string(total_ms - backend_init_ms) + " total_ms=" +
                  std::to_string(total_ms));
  if (result.error != TaskChannel::Error::kOk) {
    registry_.release(work_id);
    return result;
  }
  channels_.emplace(work_id, std::move(channel));
  result.work_id = work_id;
  return result;
}

TaskRuntime::SetupResult TaskRuntime::setup_with(
    const std::string& work_id, const std::string& request_id,
    const nlohmann::json& payload) {
  SetupResult result;
  std::lock_guard<std::mutex> lock(map_mutex_);
  if (channels_.find(work_id) != channels_.end()) {
    result.error = TaskChannel::Error::kBadState;
    return result;
  }
  if (channels_.size() >= registry_.capacity()) {
    result.error = TaskChannel::Error::kCapacity;
    return result;
  }
  const auto setup_start = std::chrono::steady_clock::now();
  auto channel = std::make_shared<TaskChannel>(work_id, backend_factory_());
  const auto backend_init_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - setup_start)
          .count();
  result.error = channel->setup(request_id, payload);
  const auto total_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - setup_start)
          .count();
  common::LogLine("runtime setup work_id=" + work_id + " backend_init_ms=" +
                  std::to_string(backend_init_ms) + " task_setup_ms=" +
                  std::to_string(total_ms - backend_init_ms) + " total_ms=" +
                  std::to_string(total_ms));
  if (result.error != TaskChannel::Error::kOk) {
    return result;
  }
  channels_.emplace(work_id, std::move(channel));
  result.work_id = work_id;
  return result;
}

TaskChannel::Error TaskRuntime::inference(
    const std::string& work_id, const std::string& request_id,
    const nlohmann::json& payload, std::chrono::milliseconds timeout,
    nlohmann::json* out, const EventSink& events) {
  std::shared_ptr<TaskChannel> channel;
  {
    std::lock_guard<std::mutex> lock(map_mutex_);
    channel = find_locked(work_id);
  }
  if (!channel) {
    return TaskChannel::Error::kNotExist;
  }
  return channel->inference(request_id, payload, timeout, out, events);
}

TaskChannel::Error TaskRuntime::cancel(const std::string& work_id,
                                       const std::string& request_id) {
  std::shared_ptr<TaskChannel> channel;
  {
    std::lock_guard<std::mutex> lock(map_mutex_);
    channel = find_locked(work_id);
  }
  if (!channel) {
    return TaskChannel::Error::kNotExist;
  }
  return channel->cancel(request_id);
}

TaskChannel::Error TaskRuntime::exit(const std::string& work_id) {
  std::shared_ptr<TaskChannel> channel;
  {
    std::lock_guard<std::mutex> lock(map_mutex_);
    const auto it = channels_.find(work_id);
    if (it == channels_.end()) {
      return TaskChannel::Error::kNotExist;
    }
    channel = it->second;
    channels_.erase(it);
    registry_.release(work_id);
  }
  return channel->exit();
}

std::optional<TaskChannel::TaskInfo> TaskRuntime::taskinfo(
    const std::string& work_id) const {
  std::shared_ptr<TaskChannel> channel;
  {
    std::lock_guard<std::mutex> lock(map_mutex_);
    channel = find_locked(work_id);
  }
  if (!channel) {
    return std::nullopt;
  }
  return channel->taskinfo();
}

bool TaskRuntime::is_alive(const std::string& work_id) const {
  std::lock_guard<std::mutex> lock(map_mutex_);
  return channels_.find(work_id) != channels_.end();
}

std::size_t TaskRuntime::size() const {
  std::lock_guard<std::mutex> lock(map_mutex_);
  return channels_.size();
}

std::shared_ptr<TaskChannel> TaskRuntime::find_locked(
    const std::string& work_id) const {
  const auto it = channels_.find(work_id);
  return it == channels_.end() ? nullptr : it->second;
}

}  // namespace slotnexus::runtime
