// PUB/SUB 流式通道：带订阅握手，避免 slow joiner 丢失首条消息。
//
// 握手流程（数据面一生产多消费的场景）：
//   1. SubSocket 先 subscribe 前缀再 connect 数据端点；
//   2. SubSocket.notify_ready(sync_endpoint) 通过配套 REQ/REP 发 "READY" 后立即返回；
//   3. PubSocket.wait_subscriber_ready() 收到 READY 并回 ACK 后，才开始 publish。
// 这样发布第一条消息时订阅关系必然已经建立，不用固定 sleep 掩盖。
// 订阅端不需要等 ACK：握手只保证"发布前订阅已建立"这一单向约束。
//
// 消息格式：两帧（topic, payload），订阅过滤由 ZMQ 在发布端完成。
#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <zmq.hpp>

namespace voxorchestra::transport {

class PubSocket {
 public:
  explicit PubSocket(zmq::context_t& ctx);
  ~PubSocket();

  PubSocket(const PubSocket&) = delete;
  PubSocket& operator=(const PubSocket&) = delete;

  void bind(const std::string& data_endpoint);
  void bind_sync(const std::string& sync_endpoint);  // 配套的握手 REP 端点
  // 阻塞等待订阅者就绪（收到 READY 并回 ACK）；超时抛 kTimeout。
  void wait_subscriber_ready(std::chrono::milliseconds timeout);
  // 发布一条消息（topic + payload 两帧）。
  void publish(const std::string& topic, const std::string& payload);
  void close();  // 幂等

 private:
  void throw_if_closed() const;

  zmq::context_t& ctx_;
  std::string data_endpoint_;
  std::string sync_endpoint_;
  bool closed_ = false;
  std::unique_ptr<zmq::socket_t> pub_;
  std::unique_ptr<zmq::socket_t> sync_rep_;
};

class SubSocket {
 public:
  explicit SubSocket(zmq::context_t& ctx);
  ~SubSocket();

  SubSocket(const SubSocket&) = delete;
  SubSocket& operator=(const SubSocket&) = delete;

  // 订阅前缀；可在 connect 之前调用（推荐顺序：先订阅再连接，
  // 避免连接建立后到订阅生效之间的消息丢失）。可多次调用。
  void subscribe(const std::string& prefix);
  void connect(const std::string& data_endpoint);
  // 通过配套 REQ 端点发送 READY 后立即返回（不等待 ACK）。
  void notify_ready(const std::string& sync_endpoint);
  // 接收一条消息；timeout 内无消息返回 false。
  bool recv(std::string& topic, std::string& payload, std::chrono::milliseconds timeout);
  void close();  // 幂等

 private:
  void throw_if_closed() const;

  zmq::context_t& ctx_;
  std::string data_endpoint_;
  bool connected_ = false;
  bool closed_ = false;
  std::unique_ptr<zmq::socket_t> sub_;
};

}  // namespace voxorchestra::transport
