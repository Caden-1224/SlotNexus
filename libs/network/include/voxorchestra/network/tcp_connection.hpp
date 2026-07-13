// TcpConnection：一条 TCP 连接的读写状态与 NDJSON 解帧。
//
// 线程归属：一个连接只属于一个 EventLoop；除 send()/close()（线程安全，
// 内部投递到 loop 线程）外，其余操作必须在 loop 线程执行。
//
// 生命周期：由 TcpServer 以 shared_ptr 持有；关闭时走 on_close 回调
// （TcpServer 从表中移除，连接随之销毁）。回调执行期间用
// shared_from_this 保护，避免回调内关闭连接导致的悬垂。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "voxorchestra/network/frame_decoder.hpp"

namespace voxorchestra::network {

class EventLoop;
class Channel;

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
 public:
  using MessageCallback =
      std::function<void(const std::shared_ptr<TcpConnection>&, const std::string& frame)>;
  using CloseCallback =
      std::function<void(const std::shared_ptr<TcpConnection>&)>;
  // 协议错误（超长帧、慢客户端写缓冲超限）：网关可先回结构化错误再关闭。
  using ProtocolErrorCallback =
      std::function<void(const std::shared_ptr<TcpConnection>&, const std::string&)>;

  TcpConnection(EventLoop* loop, int fd, MessageCallback on_message,
                CloseCallback on_close, ProtocolErrorCallback on_protocol_error);
  ~TcpConnection();

  TcpConnection(const TcpConnection&) = delete;
  TcpConnection& operator=(const TcpConnection&) = delete;

  // 线程安全：非 loop 线程调用时投递到 loop 线程执行。
  void send(const std::string& data);

  // 线程安全、幂等：关闭连接（内部投递到 loop 线程）。
  void close();

  void set_max_output_bytes(std::size_t bytes) { max_output_bytes_ = bytes; }
  std::size_t output_buffer_size() const { return output_buffer_.size(); }

  int fd() const { return fd_; }
  bool closed() const { return closed_; }

 private:
  void send_in_loop(const std::string& data);
  void handle_read();
  void handle_write();
  void handle_error();
  void close_in_loop();

  EventLoop* loop_;
  int fd_;
  bool closed_ = false;
  std::unique_ptr<Channel> channel_;
  NdjsonFrameDecoder decoder_;
  std::string output_buffer_;           // 待发送数据（未设上限则慢客户端可拖垮内存）
  std::size_t max_output_bytes_ = 1 << 20;  // 默认 1 MiB
  MessageCallback on_message_;
  CloseCallback on_close_;
  ProtocolErrorCallback on_protocol_error_;
};

}  // namespace voxorchestra::network
