#include "voxorchestra/runtime/task_runtime.hpp"

#include "voxorchestra/runtime/backends.hpp"
#include "voxorchestra/task_registry/task_registry.hpp"

#include <utility>

namespace voxorchestra::runtime {

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
                                            const std::string& payload) {
  SetupResult result;
  std::lock_guard<std::mutex> lock(map_mutex_);
  const std::string work_id = registry_.allocate();
  if (work_id.empty()) {
    result.error = TaskChannel::Error::kCapacity;
    return result;
  }
  auto channel = std::make_shared<TaskChannel>(work_id, backend_factory_());
  result.error = channel->setup(request_id, payload);
  if (result.error != TaskChannel::Error::kOk) {
    registry_.release(work_id);
    return result;
  }
  channels_.emplace(work_id, std::move(channel));
  result.work_id = work_id;
  return result;
}

TaskRuntime::SetupResult TaskRuntime::setup_with(const std::string& work_id,
                                                 const std::string& request_id,
                                                 const std::string& payload) {
  SetupResult result;
  std::lock_guard<std::mutex> lock(map_mutex_);
  if (channels_.find(work_id) != channels_.end()) {
    result.error = TaskChannel::Error::kBadState;  // 同名任务已存在
    return result;
  }
  if (channels_.size() >= registry_.capacity()) {
    result.error = TaskChannel::Error::kCapacity;
    return result;
  }
  auto channel = std::make_shared<TaskChannel>(work_id, backend_factory_());
  result.error = channel->setup(request_id, payload);
  if (result.error != TaskChannel::Error::kOk) {
    return result;
  }
  channels_.emplace(work_id, std::move(channel));
  result.work_id = work_id;
  return result;
}

TaskChannel::Error TaskRuntime::inference(const std::string& work_id,
                                          const std::string& request_id,
                                          const std::string& payload,
                                          std::chrono::milliseconds timeout,
                                          std::string* out_text,
                                          const EventSink& events) {
  std::shared_ptr<TaskChannel> channel;
  {
    std::lock_guard<std::mutex> lock(map_mutex_);
    channel = find_locked(work_id);
    if (!channel) {
      return TaskChannel::Error::kNotExist;
    }
  }
  // 解锁后调用：长时间推理不阻塞其他任务的 setup/cancel/exit。
  return channel->inference(request_id, payload, timeout, out_text, events);
}

TaskChannel::Error TaskRuntime::cancel(const std::string& work_id,
                                       const std::string& request_id) {
  std::shared_ptr<TaskChannel> channel;
  {
    std::lock_guard<std::mutex> lock(map_mutex_);
    channel = find_locked(work_id);
    if (!channel) {
      return TaskChannel::Error::kNotExist;
    }
  }
  return channel->cancel(request_id);
}

TaskChannel::Error TaskRuntime::exit(const std::string& work_id) {
  std::lock_guard<std::mutex> lock(map_mutex_);
  const auto it = channels_.find(work_id);
  if (it == channels_.end()) {
    return TaskChannel::Error::kNotExist;
  }
  const TaskChannel::Error result = it->second->exit();
  if (result == TaskChannel::Error::kOk) {
    channels_.erase(it);
    registry_.release(work_id);
  }
  return result;
}

std::optional<TaskChannel::TaskInfo> TaskRuntime::taskinfo(
    const std::string& work_id) const {
  std::lock_guard<std::mutex> lock(map_mutex_);
  const auto channel = find_locked(work_id);
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

}  // namespace voxorchestra::runtime
