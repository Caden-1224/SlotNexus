#include "voxorchestra/network/tcp_connection.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <unistd.h>

#include "voxorchestra/network/channel.hpp"
#include "voxorchestra/network/event_loop.hpp"

namespace voxorchestra::network {

namespace {

constexpr std::size_t kReadBufferBytes = 65536;

}  // namespace

TcpConnection::TcpConnection(EventLoop* loop, int fd, MessageCallback on_message,
                             CloseCallback on_close,
                             ProtocolErrorCallback on_protocol_error)
    : loop_(loop),
      fd_(fd),
      on_message_(std::move(on_message)),
      on_close_(std::move(on_close)),
      on_protocol_error_(std::move(on_protocol_error)) {
  channel_ = std::make_unique<Channel>(loop_, fd_);
  channel_->set_read_callback([this] { handle_read(); });
  channel_->set_write_callback([this] { handle_write(); });
  channel_->set_error_callback([this] { handle_error(); });
  channel_->enable_reading();
}

std::shared_ptr<TcpConnection> TcpConnection::Create(
    EventLoop* loop, int fd, MessageCallback on_message,
    CloseCallback on_close, ProtocolErrorCallback on_protocol_error) {
  auto conn = std::shared_ptr<TcpConnection>(
      new TcpConnection(loop, fd, std::move(on_message), std::move(on_close),
                        std::move(on_protocol_error)));
  // 事件分发期间保活本连接：read/error 回调（如超长帧协议错误）会关闭
  // 并销毁连接（连带释放 channel_），handle_events 后半段不能访问
  // 已释放成员（use-after-free）。构造器内 shared_from_this 不可用，
  // 故构造完成后立即绑定（期间无事件分发窗口，见头文件注释）。
  conn->channel_->set_tie(conn);
  return conn;
}

TcpConnection::~TcpConnection() {
  // 兜底：正常路径由 close_in_loop 关闭；若析构时仍未关闭（例如
  // 服务器整体销毁），直接释放 fd，不再触碰事件循环。
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

void TcpConnection::send(const std::string& data) {
  if (loop_->is_in_loop_thread()) {
    send_in_loop(data);
    return;
  }
  std::weak_ptr<TcpConnection> w = shared_from_this();
  loop_->run_in_loop([w, data] {
    if (auto sp = w.lock()) {
      sp->send_in_loop(data);
    }
  });
}

void TcpConnection::send_in_loop(const std::string& data) {
  if (closed_) {
    return;
  }
  if (output_buffer_.size() + data.size() > max_output_bytes_) {
    // 慢客户端：对方不读，写缓冲超过上限，视为协议错误并关闭。
    on_protocol_error_(shared_from_this(), "写缓冲超过上限（慢客户端）");
    close_in_loop();
    return;
  }
  output_buffer_.append(data);
  if (!channel_->is_writing()) {
    channel_->enable_writing();
  }
}

void TcpConnection::close() {
  if (loop_->is_in_loop_thread()) {
    close_in_loop();
    return;
  }
  std::weak_ptr<TcpConnection> w = shared_from_this();
  loop_->run_in_loop([w] {
    if (auto sp = w.lock()) {
      sp->close_in_loop();
    }
  });
}

void TcpConnection::handle_read() {
  const auto guard = shared_from_this();  // 回调期间连接不得销毁

  char buf[kReadBufferBytes];
  ssize_t n = 0;
  bool peer_closed = false;
  bool read_error = false;

  while (true) {
    n = ::read(fd_, buf, sizeof(buf));
    if (n > 0) {
      std::vector<std::string> frames;
      const FrameResult result = decoder_.feed(std::string_view(buf, n), frames);
      for (const std::string& frame : frames) {
        on_message_(guard, frame);
      }
      if (result == FrameResult::kOversized) {
        on_protocol_error_(guard, "单帧超过最大长度限制");
        close_in_loop();
        return;
      }
      continue;
    }
    if (n == 0) {
      peer_closed = true;
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    read_error = true;
    break;
  }

  if (peer_closed || read_error) {
    close_in_loop();
  }
}

void TcpConnection::handle_write() {
  const auto guard = shared_from_this();

  while (!output_buffer_.empty()) {
    const ssize_t n = ::write(fd_, output_buffer_.data(), output_buffer_.size());
    if (n > 0) {
      output_buffer_.erase(0, static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;  // 内核写缓冲满，保持可写监听，下次继续
    }
    // EPIPE/ECONNRESET 等：对端不可写，关闭连接。
    close_in_loop();
    return;
  }
  channel_->disable_writing();
}

void TcpConnection::handle_error() {
  const auto guard = shared_from_this();
  close_in_loop();
}

void TcpConnection::close_in_loop() {
  if (closed_) {
    return;
  }
  closed_ = true;
  channel_->remove();
  ::close(fd_);
  fd_ = -1;
  on_close_(shared_from_this());
  // 保活到当前事件分发批次结束：EventLoop 的 dispatch 循环持有本连接
  // Channel 的原始指针（tie 只保证本回调内不析构），若此处释放最后一个
  // 引用（连接销毁 → Channel 释放），批次内后续分发会访问已释放内存。
  // queue_in_loop 的任务在批次结束后才执行，届时释放。
  loop_->queue_in_loop([keep = shared_from_this()] { (void)keep; });
}

}  // namespace voxorchestra::network
