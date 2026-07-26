// EventLoop：单线程事件循环。
//
// 一个 EventLoop 由唯一线程驱动：poll 等待就绪 fd -> 分发到 Channel 回调
// -> 执行本线程投递的任务队列。跨线程投递任务用 run_in_loop：
// 写入互斥保护的队列并通过 eventfd 唤醒阻塞中的 epoll_wait。
//
// 线程归属：以调用 run() 的线程为准（允许在其他线程构造，再交给专用线程
// 运行，例如多线程 TcpServer）。run() 之前 thread_id_ 为构造线程。
//
// 生命周期：run() 在调用线程阻塞执行直到 quit()；quit() 线程安全且幂等。
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace voxorchestra::network {

class Channel;
class Poller;

class EventLoop {
 public:
  using Task = std::function<void()>;

  EventLoop();
  ~EventLoop();

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  // 在当前线程运行事件循环；quit() 后返回。
  void run();

  // 线程安全、幂等：通知循环退出并唤醒阻塞的 poll。
  void quit();

  // 线程安全：投递任务到事件循环线程。
  // 在事件循环线程内调用时立即执行，否则入队并唤醒。
  void run_in_loop(Task task);

  // 线程安全：始终入队，到当前事件分发批次结束后统一执行。
  // 用于需要把对象销毁推迟到批次结束的场景（如连接关闭保活）：
  // 分发循环持有 Channel 原始指针，批次中途销毁会让后续分发
  // 访问已释放内存（use-after-free）。
  void queue_in_loop(Task task);

  bool is_in_loop_thread() const { return std::this_thread::get_id() == thread_id_; }
  void assert_in_loop_thread() const;

  // run() 是否正在运行事件循环。
  bool running() const { return running_.load(); }

  // Channel 注册/更新/移除（必须在 loop 线程调用）。
  void update_channel(Channel* ch);
  void remove_channel(Channel* ch);

 private:
  void wakeup();         // 写 eventfd 唤醒
  void handle_wakeup();  // 读 eventfd（任务统一到批次结束后执行）
  void run_pending_tasks();

  std::thread::id thread_id_;
  std::atomic<bool> running_{false};  // run() 是否已进入循环
  std::atomic<bool> quit_{false};

  std::unique_ptr<Poller> poller_;
  int wakeup_fd_ = -1;                        // eventfd
  std::unique_ptr<Channel> wakeup_channel_;   // eventfd 的 Channel

  std::mutex task_mutex_;
  std::vector<Task> pending_tasks_;
};

}  // namespace voxorchestra::network
