// TcpServer：监听端口，接受连接并交给 TcpConnection 管理。
//
// 运行形态：构造可在任意线程，但 start()/stop() 必须归属事件循环线程
// （从外部调用时请通过 loop->run_in_loop 投递）。所有连接的回调
// （连接建立/消息/协议错误/关闭）都在 loop 线程触发。
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "voxorchestra/network/tcp_connection.hpp"

namespace voxorchestra::network {

class EventLoop;
class Channel;

class TcpServer {
 public:
  using ConnectionCallback =
      std::function<void(const std::shared_ptr<TcpConnection>&)>;
  using MessageCallback =
      std::function<void(const std::shared_ptr<TcpConnection>&, const std::string& frame)>;
  using ProtocolErrorCallback =
      std::function<void(const std::shared_ptr<TcpConnection>&, const std::string&)>;

  TcpServer(EventLoop* loop, const std::string& host, std::uint16_t port);
  ~TcpServer();

  TcpServer(const TcpServer&) = delete;
  TcpServer& operator=(const TcpServer&) = delete;

  void set_connection_callback(ConnectionCallback cb) { on_connection_ = std::move(cb); }
  void set_close_callback(ConnectionCallback cb) { on_connection_close_ = std::move(cb); }
  void set_message_callback(MessageCallback cb) { on_message_ = std::move(cb); }
  void set_protocol_error_callback(ProtocolErrorCallback cb) {
    on_protocol_error_ = std::move(cb);
  }
  void set_max_output_bytes(std::size_t bytes) { max_output_bytes_ = bytes; }

  // 创建监听 socket 并开始接受连接（必须已在 loop 线程）。
  void start();

  // 线程安全：关闭监听并断开所有连接（内部投递到 loop 线程）。
  void stop();

  // 实际绑定的端口（bind 端口 0 时由内核分配，供测试使用）。
  std::uint16_t local_port() const { return local_port_; }

  // 当前存活的连接数（仅供测试观察，须在 loop 线程读取）。
  std::size_t connection_count() const { return connections_.size(); }

 private:
  void stop_in_loop();
  void handle_accept();

  EventLoop* loop_;
  std::string host_;
  std::uint16_t port_;
  std::uint16_t local_port_ = 0;
  int listen_fd_ = -1;
  bool started_ = false;
  std::unique_ptr<Channel> accept_channel_;
  std::map<int, std::shared_ptr<TcpConnection>> connections_;
  std::size_t max_output_bytes_ = 1 << 20;

  ConnectionCallback on_connection_;
  ConnectionCallback on_connection_close_;
  MessageCallback on_message_;
  ProtocolErrorCallback on_protocol_error_;
};

}  // namespace voxorchestra::network
