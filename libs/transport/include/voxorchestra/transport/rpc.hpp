// 最小 RPC：REQ/REP 模式，所有等待带 deadline。
//
// 控制面语义（setup/cancel/taskinfo/exit）低频、需要明确结果，用本封装。
#pragma once

#include <atomic>
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

  // 异步两段式调用：call_async 发送请求后立即返回（REQ 进入等待响应阶段），
  // poll_response 在 timeout 内等待响应（收到返回 true；超时返回 false，
  // 状态机保持有效，调用方可轮询其他事件源后再调用）。
  // 用途：推理请求（长耗时）与数据面事件流并行消费——请求发出后，
  // 事件经 SUB 实时回放，响应确认推理完成。
  // 注意：call_async 后必须 poll_response 至成功（或 close），期间不可再次
  // call_async（REQ 状态机不允许重发）。发送失败抛 kSendFailed/kRecvFailed；
  // 通道已关闭抛 kClosed。
  void call_async(const std::string& request);
  bool poll_response(std::string& response, std::chrono::milliseconds timeout);

  void close();  // 幂等；关闭后 call() 抛 kClosed

 private:
  void recreate_socket();
  void throw_if_closed() const;

  zmq::context_t& ctx_;
  std::string endpoint_;
  bool connected_ = false;
  std::atomic<bool> closed_{false};
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
  // 原子标志：close() 可与服务线程的 serve_once_timeout 并发（优雅停机）；
  // socket 只在服务线程销毁，close() 不跨线程操作 zmq 对象（见实现注释）。
  std::atomic<bool> closed_{false};
  std::unique_ptr<zmq::socket_t> socket_;
};

}  // namespace voxorchestra::transport
