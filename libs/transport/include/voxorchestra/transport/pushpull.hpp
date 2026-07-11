// PUSH/PULL 任务分发通道。
//
// 数据面场景：生产者向消费者分发任务；PULL 端（worker）绑定，PUSH 端连接。
// PUSH 端带兜底发送超时，避免消费者消失时无限阻塞。
#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <zmq.hpp>

namespace voxorchestra::transport {

class PushSocket {
 public:
  explicit PushSocket(zmq::context_t& ctx);
  ~PushSocket();

  PushSocket(const PushSocket&) = delete;
  PushSocket& operator=(const PushSocket&) = delete;

  void connect(const std::string& endpoint);
  // 发送一条消息；timeout 内无法发出抛 kTimeout。
  void send(const std::string& payload, std::chrono::milliseconds timeout);
  void close();  // 幂等

 private:
  void throw_if_closed() const;

  zmq::context_t& ctx_;
  std::string endpoint_;
  bool closed_ = false;
  std::unique_ptr<zmq::socket_t> socket_;
};

class PullSocket {
 public:
  explicit PullSocket(zmq::context_t& ctx);
  ~PullSocket();

  PullSocket(const PullSocket&) = delete;
  PullSocket& operator=(const PullSocket&) = delete;

  void bind(const std::string& endpoint);
  // 接收一条消息；timeout 内无消息返回 false。
  bool recv(std::string& payload, std::chrono::milliseconds timeout);
  void close();  // 幂等

 private:
  void throw_if_closed() const;

  zmq::context_t& ctx_;
  std::string endpoint_;
  bool closed_ = false;
  std::unique_ptr<zmq::socket_t> socket_;
};

}  // namespace voxorchestra::transport
