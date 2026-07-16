#include "voxorchestra/task_registry/task_registry.hpp"

#include <mutex>
#include <unordered_set>

namespace voxorchestra::task_registry {

namespace {

constexpr std::size_t kDefaultMaxTasks = 64;

}  // namespace

class TaskRegistry::Impl {
 public:
  explicit Impl(std::size_t max_tasks)
      : max_tasks_(max_tasks == 0 ? kDefaultMaxTasks : max_tasks) {}

  std::string allocate() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (alive_.size() >= max_tasks_) {
      return {};
    }
    const std::string id = "w-" + std::to_string(next_seq_++);
    alive_.insert(id);
    return id;
  }

  bool find(const std::string& work_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return alive_.count(work_id) > 0;
  }

  bool release(const std::string& work_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return alive_.erase(work_id) > 0;
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return alive_.size();
  }

  std::size_t capacity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_tasks_;
  }

 private:
  const std::size_t max_tasks_;
  mutable std::mutex mutex_;
  std::unordered_set<std::string> alive_;
  std::size_t next_seq_ = 0;
};

TaskRegistry::TaskRegistry(std::size_t max_tasks)
    : impl_(std::make_unique<Impl>(max_tasks)) {}

TaskRegistry::~TaskRegistry() = default;

std::string TaskRegistry::allocate() { return impl_->allocate(); }

bool TaskRegistry::find(const std::string& work_id) const { return impl_->find(work_id); }

bool TaskRegistry::release(const std::string& work_id) { return impl_->release(work_id); }

std::size_t TaskRegistry::size() const { return impl_->size(); }

std::size_t TaskRegistry::capacity() const { return impl_->capacity(); }

}  // namespace voxorchestra::task_registry
