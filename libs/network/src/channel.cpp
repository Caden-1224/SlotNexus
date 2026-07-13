#include "voxorchestra/network/channel.hpp"

#include <utility>

#include "voxorchestra/network/event_loop.hpp"

namespace voxorchestra::network {

Channel::Channel(EventLoop* loop, int fd) : loop_(loop), fd_(fd) {}

Channel::~Channel() = default;

void Channel::handle_events(int received_events) {
  revents_ = received_events;

  if ((revents_ & kErrorEvent) != 0) {
    if (error_cb_) {
      error_cb_();
    }
    // 错误事件处理完即返回；错误通常是连接问题，读写回调不该再触发。
    return;
  }
  if ((revents_ & kReadEvent) != 0 && read_cb_) {
    read_cb_();
  }
  if ((revents_ & kWriteEvent) != 0 && write_cb_) {
    write_cb_();
  }
}

void Channel::remove() {
  loop_->remove_channel(this);
}

void Channel::update() {
  loop_->update_channel(this);
}

}  // namespace voxorchestra::network
