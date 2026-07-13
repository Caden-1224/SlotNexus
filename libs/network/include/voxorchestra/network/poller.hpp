// Poller：epoll（LT 模式）封装。
//
// 职责：把 Channel 的兴趣事件注册到 epoll，epoll_wait 后就绪事件
// 回填到 Channel 的 revents，并把就绪 Channel 收集到 active 列表。
#pragma once

#include <map>
#include <vector>

#include <sys/epoll.h>

namespace voxorchestra::network {

class EventLoop;
class Channel;

class Poller {
 public:
  explicit Poller(EventLoop* loop);
  ~Poller();

  Poller(const Poller&) = delete;
  Poller& operator=(const Poller&) = delete;

  // 注册/更新 Channel（首次 update 为 EPOLL_CTL_ADD，之后为 MOD）。
  void update_channel(Channel* ch);
  // 注销 Channel（EPOLL_CTL_DEL）。
  void remove_channel(Channel* ch);

  // 等待 timeout_ms（-1 表示无限），就绪的 Channel 追加到 active。
  void poll(int timeout_ms, std::vector<Channel*>& active);

  int epoll_fd() const { return epoll_fd_; }

 private:
  static constexpr int kMaxEvents = 256;

  EventLoop* loop_;
  int epoll_fd_;
  std::vector<struct epoll_event> events_;
  std::map<int, Channel*> channels_;  // fd -> Channel，用于就绪事件回查
};

}  // namespace voxorchestra::network
