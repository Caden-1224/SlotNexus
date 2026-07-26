// Channel：一个 fd 与其事件回调的绑定。
//
// 线程归属：一个 Channel 只属于一个 EventLoop；所有修改（enable/disable、
// remove）都必须在该 EventLoop 线程执行（EventLoop::assert_in_loop_thread）。
// 回调由 EventLoop 在 poll 返回后调用，因此回调内也必然在所属线程。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace voxorchestra::network {

class EventLoop;

// 抽象事件标志（与具体 poll 机制解耦）。
enum ChannelEvent : int {
  kReadEvent = 1,   // 可读
  kWriteEvent = 2,  // 可写
  kErrorEvent = 4,  // 错误/对端关闭（EPOLLERR/EPOLLHUP）
};

class Channel {
 public:
  using ReadCallback = std::function<void()>;
  using WriteCallback = std::function<void()>;
  using ErrorCallback = std::function<void()>;

  Channel(EventLoop* loop, int fd);
  ~Channel();

  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;

  int fd() const { return fd_; }
  int events() const { return events_; }

  void set_read_callback(ReadCallback cb) { read_cb_ = std::move(cb); }
  void set_write_callback(WriteCallback cb) { write_cb_ = std::move(cb); }
  void set_error_callback(ErrorCallback cb) { error_cb_ = std::move(cb); }

  void enable_reading() { events_ |= kReadEvent; update(); }
  void disable_reading() { events_ &= ~kReadEvent; update(); }
  void enable_writing() { events_ |= kWriteEvent; update(); }
  void disable_writing() { events_ &= ~kWriteEvent; update(); }
  bool is_writing() const { return (events_ & kWriteEvent) != 0; }

  // 由 EventLoop 调用：按 revents 分发到对应回调。
  void handle_events(int received_events);

  // 绑定归属对象（如 TcpConnection）：事件分发期间持有一个引用，
  // 防止回调关闭连接并销毁归属对象（连带释放本 Channel）后，
  // handle_events 继续访问已释放的成员（use-after-free）。
  void set_tie(const std::shared_ptr<void>& tie) {
    tie_ = tie;
    tied_ = true;
  }

  // 从所属 EventLoop/Poller 注销（必须在该 loop 线程调用）。
  void remove();

  EventLoop* loop() const { return loop_; }

  // ---- 仅供 Poller 使用 ----
  void set_revents(int revents) { revents_ = revents; }
  int revents() const { return revents_; }
  void set_added_to_poller(bool v) { added_to_poller_ = v; }
  bool added_to_poller() const { return added_to_poller_; }

 private:
  void update();

  EventLoop* loop_;
  int fd_;
  int events_ = 0;        // 期望监听的事件
  int revents_ = 0;       // 本次就绪的事件
  bool added_to_poller_ = false;
  std::weak_ptr<void> tie_;  // 归属对象弱引用（事件分发期间保活）
  bool tied_ = false;
  ReadCallback read_cb_;
  WriteCallback write_cb_;
  ErrorCallback error_cb_;
};

}  // namespace voxorchestra::network
