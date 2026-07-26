#include "voxorchestra/network/tcp_server.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "voxorchestra/network/channel.hpp"
#include "voxorchestra/network/event_loop.hpp"

namespace voxorchestra::network {

TcpServer::TcpServer(EventLoop* loop, const std::string& host, std::uint16_t port)
    : loop_(loop), host_(host), port_(port) {}

TcpServer::~TcpServer() {
  // 兜底：正常路径由 stop() 清理；若 loop 已停止而服务器未停，直接关监听 fd。
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
}

void TcpServer::start() {
  loop_->assert_in_loop_thread();
  if (started_) {
    return;
  }
  started_ = true;

  listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) {
    throw std::runtime_error(std::string("socket 创建失败: ") + std::strerror(errno));
  }

  int reuse = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (host_ != "127.0.0.1" && host_ != "localhost") {
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
      throw std::runtime_error("非法监听地址: " + host_);
    }
  }
  addr.sin_port = htons(port_);

  if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    throw std::runtime_error(std::string("bind 失败: ") + std::strerror(errno));
  }
  if (::listen(listen_fd_, 128) < 0) {
    throw std::runtime_error(std::string("listen 失败: ") + std::strerror(errno));
  }

  // 记录实际端口（bind 端口 0 时由内核分配）。
  struct sockaddr_in bound {};
  socklen_t bound_len = sizeof(bound);
  ::getsockname(listen_fd_, reinterpret_cast<struct sockaddr*>(&bound), &bound_len);
  local_port_ = ntohs(bound.sin_port);

  accept_channel_ = std::make_unique<Channel>(loop_, listen_fd_);
  accept_channel_->set_read_callback([this] { handle_accept(); });
  accept_channel_->set_error_callback([this] { handle_accept(); });
  accept_channel_->enable_reading();
}

void TcpServer::stop() {
  if (loop_->is_in_loop_thread()) {
    stop_in_loop();
    return;
  }
  loop_->run_in_loop([this] { stop_in_loop(); });
}

void TcpServer::stop_in_loop() {
  if (!started_) {
    return;
  }
  started_ = false;

  if (accept_channel_) {
    accept_channel_->remove();
    accept_channel_.reset();
  }
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }

  // 断开所有连接（拷贝一份，避免回调中修改 map）。
  std::vector<std::shared_ptr<TcpConnection>> conns;
  for (const auto& [fd, conn] : connections_) {
    conns.push_back(conn);
  }
  connections_.clear();
  for (const auto& conn : conns) {
    conn->close();
  }
}

void TcpServer::handle_accept() {
  while (true) {
    const int conn_fd =
        ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (conn_fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;  // 本轮已全部接受
      }
      if (errno == EINTR) {
        continue;
      }
      break;  // 其他错误：记录并停止接受
    }

    auto conn = TcpConnection::Create(
        loop_, conn_fd,
        [this](const std::shared_ptr<TcpConnection>& c, const std::string& f) {
          if (on_message_) {
            on_message_(c, f);
          }
        },
        // 按创建时的原始 fd 擦除：close_in_loop 会在回调前把 c->fd()
        // 置为 -1，若用 c->fd() 擦除会恒为 no-op，导致连接对象泄漏。
        [this, conn_fd](const std::shared_ptr<TcpConnection>& c) {
          connections_.erase(conn_fd);
          if (on_connection_close_) {
            on_connection_close_(c);
          }
        },
        [this](const std::shared_ptr<TcpConnection>& c, const std::string& m) {
          if (on_protocol_error_) {
            on_protocol_error_(c, m);
          }
        });
    conn->set_max_output_bytes(max_output_bytes_);
    connections_[conn_fd] = conn;
    if (on_connection_) {
      on_connection_(conn);
    }
  }
}

}  // namespace voxorchestra::network
