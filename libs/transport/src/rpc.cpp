#include "voxorchestra/transport/rpc.hpp"

#include <cerrno>
#include <chrono>
#include <utility>

#include "voxorchestra/transport/transport_error.hpp"

// RpcServer 线程模型（TSan 回归发现并修复，Mock 冻结阶段）：
//   serve_once_timeout 在服务线程运行，close() 可在任意线程调用（优雅停机）。
//   zmq socket 非线程安全：socket 只允许在服务线程销毁，close() 只置原子
//   标志；服务线程在下一轮 serve_once_timeout 入口观察到 closed_ 后自行
//   销毁 socket（此后不再触碰），或由析构在服务线程退出后兜底。
//   recv 带 timeout，close() 后最迟一个 timeout 窗口内服务循环退出。

namespace voxorchestra::transport {

namespace {

// 把 cppzmq 抛出的错误映射为 TransportError；EAGAIN 由调用方先处理（返回 false）。
TransportError make_error(zmq::error_t& e) {
  if (e.num() == ETERM) {
    return TransportError(TransportErrorCode::kClosed, e.what());
  }
  if (e.num() == EINTR) {
    return TransportError(TransportErrorCode::kInterrupted, e.what());
  }
  return TransportError(TransportErrorCode::kRecvFailed, e.what());
}

}  // namespace

// ---------- RpcClient ----------

RpcClient::RpcClient(zmq::context_t& ctx) : ctx_(ctx) {}

RpcClient::~RpcClient() { close(); }

void RpcClient::connect(const std::string& endpoint) {
  throw_if_closed();
  endpoint_ = endpoint;
  recreate_socket();
  socket_->connect(endpoint_);
  connected_ = true;
}

void RpcClient::recreate_socket() {
  socket_ = std::make_unique<zmq::socket_t>(ctx_, zmq::socket_type::req);
  // 兜底发送超时：慢服务端不应无限阻塞调用方。
  const int kDefaultSendTimeoutMs = 3000;
  socket_->set(zmq::sockopt::sndtimeo, kDefaultSendTimeoutMs);
  // 关闭时立即丢弃滞留状态：避免 REQ socket 停在死端点的重连轮询，
  // 导致 zmq_ctx_term 永久等待 io 线程退出。
  socket_->set(zmq::sockopt::linger, 0);
}

std::string RpcClient::call(const std::string& request,
                            std::chrono::milliseconds deadline) {
  throw_if_closed();
  if (!connected_ || !socket_) {
    throw TransportError(TransportErrorCode::kRecvFailed,
                         "RpcClient 未 connect");
  }

  // REQ 状态机要求：超时后重建 socket，下一次 call 才能重新发请求。
  if (deadline <= std::chrono::milliseconds::zero()) {
    recreate_socket();
    socket_->connect(endpoint_);
    throw TransportError(TransportErrorCode::kTimeout, "RPC 超时（deadline 为 0）");
  }

  try {
    socket_->send(zmq::buffer(request), zmq::send_flags::none);
  } catch (zmq::error_t& e) {
    throw make_error(e);
  }

  const int timeout_ms = static_cast<int>(deadline.count());
  socket_->set(zmq::sockopt::rcvtimeo, timeout_ms);

  zmq::message_t reply;
  bool received = false;
  try {
    received = socket_->recv(reply, zmq::recv_flags::none).has_value();
  } catch (zmq::error_t& e) {
    throw make_error(e);
  }

  if (!received) {
    // 状态机已失效：重建并重连，允许调用方重试。
    recreate_socket();
    socket_->connect(endpoint_);
    throw TransportError(TransportErrorCode::kTimeout,
                         "RPC 在 " + std::to_string(timeout_ms) + "ms 内未收到响应");
  }

  return reply.to_string();
}

void RpcClient::call_async(const std::string& request) {
  throw_if_closed();
  if (!connected_ || !socket_) {
    throw TransportError(TransportErrorCode::kRecvFailed,
                         "RpcClient 未 connect");
  }
  try {
    socket_->send(zmq::buffer(request), zmq::send_flags::none);
  } catch (zmq::error_t& e) {
    throw make_error(e);
  }
  // 发送后 REQ 进入等待响应阶段：后续 poll_response 消费响应。
}

bool RpcClient::poll_response(std::string& response,
                              std::chrono::milliseconds timeout) {
  throw_if_closed();
  if (!connected_ || !socket_) {
    throw TransportError(TransportErrorCode::kRecvFailed,
                         "RpcClient 未 connect");
  }
  if (timeout < std::chrono::milliseconds::zero()) {
    timeout = std::chrono::milliseconds::zero();
  }
  socket_->set(zmq::sockopt::rcvtimeo, static_cast<int>(timeout.count()));
  zmq::message_t reply;
  bool received = false;
  try {
    received = socket_->recv(reply, zmq::recv_flags::none).has_value();
  } catch (zmq::error_t& e) {
    throw make_error(e);
  }
  if (!received) {
    // 超时：REQ 仍在等待响应阶段，状态机有效，可再次 poll_response。
    return false;
  }
  response = reply.to_string();
  return true;
}

void RpcClient::close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  connected_ = false;
  socket_.reset();
}

void RpcClient::throw_if_closed() const {
  if (closed_) {
    throw TransportError(TransportErrorCode::kClosed, "RpcClient 已关闭");
  }
}

// ---------- RpcServer ----------

RpcServer::RpcServer(zmq::context_t& ctx) : ctx_(ctx) {}

RpcServer::~RpcServer() { close(); }

void RpcServer::bind(const std::string& endpoint) {
  throw_if_closed();
  endpoint_ = endpoint;
  socket_ = std::make_unique<zmq::socket_t>(ctx_, zmq::socket_type::rep);
  socket_->bind(endpoint_);
}

bool RpcServer::serve_once_timeout(const Handler& h,
                                   std::chrono::milliseconds timeout) {
  if (closed_.load()) {
    // 服务线程销毁 socket：close() 已置标志且不跨线程操作 zmq 对象，
    // 在此回收（此后本线程也不再使用 socket_）。
    socket_.reset();
    throw TransportError(TransportErrorCode::kClosed, "RpcServer 已关闭");
  }
  if (!socket_) {
    throw TransportError(TransportErrorCode::kRecvFailed,
                         "RpcServer 未 bind");
  }

  socket_->set(zmq::sockopt::rcvtimeo, static_cast<int>(timeout.count()));

  zmq::message_t request;
  bool received = false;
  try {
    received = socket_->recv(request, zmq::recv_flags::none).has_value();
  } catch (zmq::error_t& e) {
    if (e.num() == EINTR) {
      // 被信号中断（如 SIGTERM 优雅退出）：按无请求处理返回 false，
      // 调用方的服务循环会重新轮询并检查退出标志。
      return false;
    }
    throw make_error(e);
  }
  if (!received) {
    return false;
  }

  const std::string reply = h(request.to_string());
  try {
    socket_->send(zmq::buffer(reply), zmq::send_flags::none);
  } catch (zmq::error_t& e) {
    throw make_error(e);
  }
  return true;
}

void RpcServer::close() {
  if (closed_.exchange(true)) {
    return;
  }
  // 不在此销毁 socket：服务线程可能正阻塞在 recv，跨线程 zmq 操作是
  // 未定义行为。socket 由服务线程在 serve_once_timeout 入口回收，
  // 或由析构在服务线程退出后兜底。
}

void RpcServer::throw_if_closed() const {
  if (closed_) {
    throw TransportError(TransportErrorCode::kClosed, "RpcServer 已关闭");
  }
}

}  // namespace voxorchestra::transport
