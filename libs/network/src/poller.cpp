#include "voxorchestra/network/poller.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <unistd.h>

#include "voxorchestra/network/channel.hpp"

namespace voxorchestra::network {

namespace {

// 把 epoll 就绪标志映射为抽象的 ChannelEvent。
int to_channel_events(uint32_t epoll_events) {
  int e = 0;
  if (epoll_events & (EPOLLIN | EPOLLPRI)) {
    e |= kReadEvent;
  }
  if (epoll_events & EPOLLOUT) {
    e |= kWriteEvent;
  }
  // EPOLLHUP/EPOLLERR 必须由用户处理（如对端关闭、socket 错误）。
  if (epoll_events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
    e |= kErrorEvent;
  }
  return e;
}

uint32_t to_epoll_events(int channel_events) {
  uint32_t e = 0;
  if (channel_events & kReadEvent) {
    e |= EPOLLIN;
  }
  if (channel_events & kWriteEvent) {
    e |= EPOLLOUT;
  }
  return e;
}

}  // namespace

Poller::Poller(EventLoop* loop) : loop_(loop), events_(kMaxEvents) {
  epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) {
    throw std::runtime_error(std::string("epoll_create1 失败: ") +
                             std::strerror(errno));
  }
}

Poller::~Poller() {
  ::close(epoll_fd_);
}

void Poller::update_channel(Channel* ch) {
  const int fd = ch->fd();
  struct epoll_event ev {};
  ev.events = to_epoll_events(ch->events());
  ev.data.fd = fd;

  const int op = ch->added_to_poller() ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
  if (::epoll_ctl(epoll_fd_, op, fd, &ev) < 0) {
    throw std::runtime_error(std::string("epoll_ctl ") +
                             (op == EPOLL_CTL_ADD ? "ADD" : "MOD") +
                             " 失败: " + std::strerror(errno));
  }
  ch->set_added_to_poller(true);
  channels_[fd] = ch;
}

void Poller::remove_channel(Channel* ch) {
  const int fd = ch->fd();
  // 即使之前未注册也尝试 DEL 是安全的；但按状态判断避免误删同 fd 的新 Channel。
  if (ch->added_to_poller()) {
    struct epoll_event ev {};
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, &ev) < 0 && errno != ENOENT) {
      throw std::runtime_error(std::string("epoll_ctl DEL 失败: ") +
                               std::strerror(errno));
    }
    ch->set_added_to_poller(false);
  }
  channels_.erase(fd);
}

void Poller::poll(int timeout_ms, std::vector<Channel*>& active) {
  const int n = ::epoll_wait(epoll_fd_, events_.data(),
                             static_cast<int>(events_.size()), timeout_ms);
  if (n < 0) {
    if (errno == EINTR) {
      return;  // 被信号中断：返回空，由 EventLoop 继续循环
    }
    throw std::runtime_error(std::string("epoll_wait 失败: ") +
                             std::strerror(errno));
  }

  for (int i = 0; i < n; ++i) {
    const int fd = events_[i].data.fd;
    const auto it = channels_.find(fd);
    if (it == channels_.end()) {
      continue;  // 事件返回时已被移除（如回调中关闭了连接）
    }
    Channel* ch = it->second;
    ch->set_revents(to_channel_events(events_[i].events));
    active.push_back(ch);
  }
}

}  // namespace voxorchestra::network
