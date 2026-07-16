// TaskRegistry：任务实例（work_id）的分配、查询与释放。
//
// work_id 标识一个任务实例（如一个语音会话对应的推理链）；
// request_id 标识任务内的一次请求，由上层消息携带，不在此注册。
//
// 语义保证：
//   - work_id 单调递增（w-1, w-2, ...），释放后不回收复用，避免
//     旧请求引用到新任务的 ABA 问题；
//   - release 幂等：释放不存在的 id 返回 false，不抛异常；
//   - 有容量上限（防 4 GB 板上资源失控），耗尽时 allocate 返回空串。
#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace voxorchestra::task_registry {

class TaskRegistry {
 public:
  // max_tasks：并发任务上限，0 表示使用默认值。
  explicit TaskRegistry(std::size_t max_tasks = 0);
  ~TaskRegistry();

  TaskRegistry(const TaskRegistry&) = delete;
  TaskRegistry& operator=(const TaskRegistry&) = delete;

  // 分配新 work_id；达到容量上限时返回空字符串。
  std::string allocate();

  // 查询：id 存在且未释放返回 true。
  bool find(const std::string& work_id) const;

  // 释放：成功返回 true；id 不存在（含重复释放）返回 false。
  bool release(const std::string& work_id);

  // 当前存活任务数。
  std::size_t size() const;

  // 容量上限（0 表示默认值，见构造）。
  std::size_t capacity() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace voxorchestra::task_registry
