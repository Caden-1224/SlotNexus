// EdgeGateway：外部客户端（voice_cli / 测试）的 TCP 接入点。
//
// 职责：
//   1. 接收 NDJSON 请求，解析为 MessageEnvelope（复用 protocol 校验）；
//   2. 合法 action 请求（setup/inference/cancel/taskinfo/exit）转发给
//      Unit Manager 执行，并把响应（ack/error）送回原连接；
//   3. 客户端方向不允许的类型（event/ack/error）回结构化错误信封；
//   4. 非法请求（坏 JSON/未知版本/未知类型/缺字段）回结构化错误信封，
//      连接保持可用；
//   5. 超长帧由连接层关闭（不可信客户端不回复）。
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <zmq.hpp>

#include "voxorchestra/network/event_loop.hpp"
#include "voxorchestra/network/tcp_server.hpp"
#include "voxorchestra/transport/rpc.hpp"

namespace voxorchestra::gateway {

class EdgeGateway {
 public:
  // manager_endpoint：Unit Manager 的 RPC 端点。
  // forward_deadline：转发给 Manager 的 RPC 等待上限（默认 3000 ms；
  // 硬件后端模型加载可能数秒，经 CLI 调大）。
  EdgeGateway(network::EventLoop* loop, const std::string& host,
              std::uint16_t port,
              const std::string& manager_endpoint = "tcp://127.0.0.1:19100",
              std::chrono::milliseconds forward_deadline =
                  std::chrono::milliseconds(3000));
  ~EdgeGateway();

  EdgeGateway(const EdgeGateway&) = delete;
  EdgeGateway& operator=(const EdgeGateway&) = delete;

  // 在 loop 线程创建 zmq 对象并开始监听。
  void start();
  // 线程安全：在 loop 线程销毁 zmq 对象并停止监听。
  void stop();

  std::uint16_t local_port() const { return server_.local_port(); }

 private:
  void handle_message(const std::shared_ptr<network::TcpConnection>& conn,
                      const std::string& frame);
  void handle_protocol_error(const std::shared_ptr<network::TcpConnection>& conn,
                             const std::string& message);
  // 转发给 Manager；不可达时回 manager_unreachable 错误信封。
  void forward_to_manager(const std::shared_ptr<network::TcpConnection>& conn,
                          const std::string& frame);

  network::EventLoop* loop_;
  network::TcpServer server_;
  std::string manager_endpoint_;
  std::chrono::milliseconds forward_deadline_;
  // zmq 对象与 loop 线程同生共死：创建于 start()（loop 线程），销毁于
  // stop() 投递的 loop 任务。zmq 不跨线程创建/销毁；若在 loop 线程上
  // 关闭 REQ socket，io 线程能及时处理 close，避免死端点重连轮询与
  // zmq_ctx_term 的永久等待。
  std::unique_ptr<zmq::context_t> ctx_;
  std::unique_ptr<transport::RpcClient> manager_;
};

}  // namespace voxorchestra::gateway
