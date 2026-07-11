#include "voxorchestra/transport/rpc.hpp"

#include <cerrno>
#include <chrono>
#include <utility>

#include "voxorchestra/transport/transport_error.hpp"

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
  throw_if_closed();
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
  if (closed_) {
    return;
  }
  closed_ = true;
  socket_.reset();
}

void RpcServer::throw_if_closed() const {
  if (closed_) {
    throw TransportError(TransportErrorCode::kClosed, "RpcServer 已关闭");
  }
}

}  // namespace voxorchestra::transport
