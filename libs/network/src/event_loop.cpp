#include "voxorchestra/network/event_loop.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <sys/eventfd.h>
#include <unistd.h>

#include "voxorchestra/network/channel.hpp"
#include "voxorchestra/network/poller.hpp"

namespace voxorchestra::network {

EventLoop::EventLoop() {
  thread_id_ = std::this_thread::get_id();
  poller_ = std::make_unique<Poller>(this);

  wakeup_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wakeup_fd_ < 0) {
    throw std::runtime_error(std::string("eventfd 失败: ") +
                             std::strerror(errno));
  }

  wakeup_channel_ = std::make_unique<Channel>(this, wakeup_fd_);
  wakeup_channel_->set_read_callback([this] { handle_wakeup(); });
  wakeup_channel_->enable_reading();
}

EventLoop::~EventLoop() {
  wakeup_channel_.reset();  // 先从 poller 注销
  ::close(wakeup_fd_);
}

void EventLoop::run() {
  // 线程归属以调用 run() 的线程为准：允许在别的线程构造，再交给专用线程运行
  // （TcpServer 多线程模式常用此形态）。
  thread_id_ = std::this_thread::get_id();
  running_.store(true);

  while (!quit_.load()) {
    std::vector<Channel*> active;
    poller_->poll(-1, active);

    for (Channel* ch : active) {
      ch->handle_events(ch->revents());
    }
    run_pending_tasks();
  }

  running_.store(false);
}

void EventLoop::quit() {
  quit_.store(true);
  wakeup();  // 确保阻塞在 epoll_wait 的循环被唤醒并检查退出标志
}

void EventLoop::run_in_loop(Task task) {
  // 仅当确实运行在事件循环内时才同步执行；run() 之前的投递一律入队，
  // 由 run() 启动后在事件循环线程执行。
  if (running_.load() && is_in_loop_thread()) {
    task();
    return;
  }
  {
    std::lock_guard<std::mutex> lock(task_mutex_);
    pending_tasks_.push_back(std::move(task));
  }
  wakeup();
}

void EventLoop::assert_in_loop_thread() const {
  if (!is_in_loop_thread()) {
    throw std::runtime_error("EventLoop 跨线程操作：必须在所属 loop 线程调用");
  }
}

void EventLoop::update_channel(Channel* ch) {
  assert_in_loop_thread();
  poller_->update_channel(ch);
}

void EventLoop::remove_channel(Channel* ch) {
  assert_in_loop_thread();
  poller_->remove_channel(ch);
}

void EventLoop::wakeup() {
  const uint64_t one = 1;
  const ssize_t n = ::write(wakeup_fd_, &one, sizeof(one));
  if (n != sizeof(one) && errno != EAGAIN) {
    // eventfd 非阻塞写：满时 EAGAIN 表示已有人唤醒，可忽略。
    throw std::runtime_error(std::string("eventfd 唤醒写失败: ") +
                             std::strerror(errno));
  }
}

void EventLoop::handle_wakeup() {
  // 读空 eventfd 计数器，避免一直触发可读。
  uint64_t value = 0;
  while (::read(wakeup_fd_, &value, sizeof(value)) > 0) {
  }
  run_pending_tasks();
}

void EventLoop::run_pending_tasks() {
  std::vector<Task> tasks;
  {
    std::lock_guard<std::mutex> lock(task_mutex_);
    tasks.swap(pending_tasks_);
  }
  for (Task& t : tasks) {
    t();
  }
}

}  // namespace voxorchestra::network
