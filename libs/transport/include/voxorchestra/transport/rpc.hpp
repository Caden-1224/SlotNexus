// 最小 RPC：REQ/REP 模式，所有等待带 deadline。
//
// 控制面语义（setup/cancel/taskinfo/exit）低频、需要明确结果，用本封装。
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <zmq.hpp>

namespace voxorchestra::transport {

// REQ/REP 客户端。
//
// 注意：REQ socket 是严格状态机（发->收->发->收），超时会使状态机失效，
// 因此 call() 超时后内部重建 socket，下一次调用可直接重试。
class RpcClient {
 public:
  // ctx 必须比本对象活得久；close() 幂等。
  explicit RpcClient(zmq::context_t& ctx);
  ~RpcClient();

  RpcClient(const RpcClient&) = delete;
  RpcClient& operator=(const RpcClient&) = delete;

  void connect(const std::string& endpoint);

  // 发送 request 并等待响应，deadline 内未收到抛 TransportError(kTimeout)。
  // 通道已关闭抛 kClosed；发送/接收失败抛 kSendFailed/kRecvFailed。
  std::string call(const std::string& request, std::chrono::milliseconds deadline);

  void close();  // 幂等；关闭后 call() 抛 kClosed

 private:
  void recreate_socket();
  void throw_if_closed() const;

  zmq::context_t& ctx_;
  std::string endpoint_;
  bool connected_ = false;
  bool closed_ = false;
  std::unique_ptr<zmq::socket_t> socket_;
};

// REQ/REP 服务端。
//
// REP 同样是严格状态机：每收到一条请求必须回复一条响应。Handler 抛异常时
// 本类向上抛出且不回复（此时应重建服务端），调用方负责保证 Handler 不抛。
class RpcServer {
 public:
  using Handler = std::function<std::string(const std::string& request)>;

  explicit RpcServer(zmq::context_t& ctx);
  ~RpcServer();

  RpcServer(const RpcServer&) = delete;
  RpcServer& operator=(const RpcServer&) = delete;

  void bind(const std::string& endpoint);

  // 等待并处理一条请求；timeout 内没有请求返回 false（含被信号中断，
  // 此时调用方应重新轮询），收到则返回 true。
  bool serve_once_timeout(const Handler& h, std::chrono::milliseconds timeout);

  void close();  // 幂等；关闭后 serve_* 抛 kClosed

 private:
  void throw_if_closed() const;

  zmq::context_t& ctx_;
  std::string endpoint_;
  bool closed_ = false;
  std::unique_ptr<zmq::socket_t> socket_;
};

}  // namespace voxorchestra::transport
