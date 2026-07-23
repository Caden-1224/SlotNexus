// 有界队列：容量受限、超时、关闭与清空语义（背压基础组件）。
//
// 语义（满队列行为必须明确，四种之一：阻塞 / 超时 / 丢弃 / 拒绝）：
//   - push / push_timeout：容量满时阻塞，最多等待超时时间；仍满则
//     返回 kFull（由调用方决定丢弃或重试，丢弃必须计数）；
//   - try_push：非阻塞，满立即返回 kFull（拒绝）；
//   - pop / pop_timeout：队列空时阻塞，最多等待超时时间；仍空返回 kEmpty；
//   - close：幂等；关闭后唤醒全部等待者，pop 返回 kClosed，push 返回 kClosed，
//     已有数据保留可继续 pop（先 drain 语义）；
//   - clear：清空全部数据并唤醒生产者（取消路径使用）；
//   - 峰值统计：peak() 返回历史最大 size，供“队列峰值 ≤ 容量”验收。
//
// 线程安全：内部互斥量 + 条件变量；谓词等待（spurious wakeup 安全），
// 同一队列可被多生产者/多消费者并发使用。
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace voxorchestra::common {

// 一次队列操作的结果。
enum class QueueResult {
  kOk,      // 成功
  kFull,    // 已满（try_push / push 超时）
  kEmpty,   // 已空（pop 超时）
  kClosed,  // 队列已关闭
};

template <typename T>
class BoundedQueue {
 public:
  // capacity 为 0 视为 1（容量必须为正，防止除零与空容量误配置）。
  explicit BoundedQueue(std::size_t capacity)
      : capacity_(capacity == 0 ? 1 : capacity) {}

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;

  // 满时阻塞直到有空间或队列关闭；关闭返回 false。
  bool push(const T& item) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_not_full_.wait(lock, [this] { return closed_ || size_ < capacity_; });
    if (closed_) {
      return false;
    }
    push_locked(item);
    return true;
  }

  // 满时最多等待 timeout；超时返回 kFull；关闭返回 kClosed。
  QueueResult push_timeout(const T& item, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_) {
      return QueueResult::kClosed;
    }
    if (size_ >= capacity_ &&
        !cv_not_full_.wait_for(lock, timeout,
                               [this] { return closed_ || size_ < capacity_; })) {
      return QueueResult::kFull;
    }
    if (closed_) {
      return QueueResult::kClosed;
    }
    push_locked(item);
    return QueueResult::kOk;
  }

  // 非阻塞：满立即返回 kFull；关闭返回 kClosed。
  QueueResult try_push(const T& item) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return QueueResult::kClosed;
    }
    if (size_ >= capacity_) {
      return QueueResult::kFull;
    }
    push_locked(item);
    return QueueResult::kOk;
  }

  // 空时阻塞直到有数据或关闭；关闭返回 false。
  bool pop(T& out) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_not_empty_.wait(lock, [this] { return closed_ || !items_.empty(); });
    if (items_.empty()) {  // 关闭后已 drain 完 → kClosed
      return false;
    }
    pop_locked(out);
    return true;
  }

  // 空时最多等待 timeout；超时返回 kEmpty；关闭返回 kClosed。
  QueueResult pop_timeout(T& out, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (items_.empty() && !closed_ &&
        !cv_not_empty_.wait_for(lock, timeout,
                                [this] { return closed_ || !items_.empty(); })) {
      return QueueResult::kEmpty;
    }
    if (items_.empty()) {  // 关闭且无数据
      return QueueResult::kClosed;
    }
    pop_locked(out);
    return QueueResult::kOk;
  }

  // 非阻塞：空立即返回 kEmpty；关闭且无数据返回 kClosed。
  QueueResult try_pop(T& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (items_.empty()) {
      return closed_ ? QueueResult::kClosed : QueueResult::kEmpty;
    }
    pop_locked(out);
    return QueueResult::kOk;
  }

  // 清空全部数据并唤醒阻塞中的生产者（取消路径）；返回被清空的条数。
  std::size_t clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t n = items_.size();
    items_.clear();
    size_ = 0;
    cv_not_full_.notify_all();
    return n;
  }

  // 关闭队列：幂等；唤醒全部等待者；已有数据保留，可继续 pop 至空。
  void close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    cv_not_full_.notify_all();
    cv_not_empty_.notify_all();
  }

  // 取走全部剩余数据（drain），等价于 clear 但返回数据本身。
  std::vector<T> drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<T> out;
    out.reserve(items_.size());
    while (!items_.empty()) {
      out.push_back(std::move(items_.front()));
      items_.pop_front();
    }
    size_ = 0;
    cv_not_full_.notify_all();
    return out;
  }

  bool empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.empty();
  }

  bool closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
  }

  std::size_t capacity() const { return capacity_; }

  // 历史最大队列深度（背压验收指标）。
  std::size_t peak() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peak_;
  }

 private:
  // 前置条件：持有 mutex_。
  void push_locked(const T& item) {
    items_.push_back(item);
    ++size_;
    if (size_ > peak_) {
      peak_ = size_;
    }
    cv_not_empty_.notify_one();
  }

  // 前置条件：持有 mutex_ 且 items_ 非空。
  void pop_locked(T& out) {
    out = std::move(items_.front());
    items_.pop_front();
    --size_;
    cv_not_full_.notify_one();
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_not_empty_;
  std::condition_variable cv_not_full_;
  std::deque<T> items_;
  const std::size_t capacity_;
  std::size_t size_ = 0;
  std::size_t peak_ = 0;
  bool closed_ = false;
};

}  // namespace voxorchestra::common
