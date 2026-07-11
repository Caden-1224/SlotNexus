#include "voxorchestra/transport/pushpull.hpp"

#include <cerrno>
#include <utility>

#include "voxorchestra/transport/transport_error.hpp"

namespace voxorchestra::transport {

namespace {

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

// ---------- PushSocket ----------

PushSocket::PushSocket(zmq::context_t& ctx) : ctx_(ctx) {}

PushSocket::~PushSocket() { close(); }

void PushSocket::connect(const std::string& endpoint) {
  throw_if_closed();
  endpoint_ = endpoint;
  socket_ = std::make_unique<zmq::socket_t>(ctx_, zmq::socket_type::push);
  socket_->connect(endpoint_);
}

void PushSocket::send(const std::string& payload, std::chrono::milliseconds timeout) {
  throw_if_closed();
  if (!socket_) {
    throw TransportError(TransportErrorCode::kRecvFailed,
                         "PushSocket 未 connect");
  }
  socket_->set(zmq::sockopt::sndtimeo, static_cast<int>(timeout.count()));
  try {
    socket_->send(zmq::buffer(payload), zmq::send_flags::none);
  } catch (zmq::error_t& e) {
    throw make_error(e);
  }
}

void PushSocket::close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  socket_.reset();
}

void PushSocket::throw_if_closed() const {
  if (closed_) {
    throw TransportError(TransportErrorCode::kClosed, "PushSocket 已关闭");
  }
}

// ---------- PullSocket ----------

PullSocket::PullSocket(zmq::context_t& ctx) : ctx_(ctx) {}

PullSocket::~PullSocket() { close(); }

void PullSocket::bind(const std::string& endpoint) {
  throw_if_closed();
  endpoint_ = endpoint;
  socket_ = std::make_unique<zmq::socket_t>(ctx_, zmq::socket_type::pull);
  socket_->bind(endpoint_);
}

bool PullSocket::recv(std::string& payload, std::chrono::milliseconds timeout) {
  throw_if_closed();
  if (!socket_) {
    throw TransportError(TransportErrorCode::kRecvFailed,
                         "PullSocket 未 bind");
  }
  socket_->set(zmq::sockopt::rcvtimeo, static_cast<int>(timeout.count()));

  zmq::message_t msg;
  bool received = false;
  try {
    received = socket_->recv(msg, zmq::recv_flags::none).has_value();
  } catch (zmq::error_t& e) {
    throw make_error(e);
  }
  if (!received) {
    return false;
  }
  payload = msg.to_string();
  return true;
}

void PullSocket::close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  socket_.reset();
}

void PullSocket::throw_if_closed() const {
  if (closed_) {
    throw TransportError(TransportErrorCode::kClosed, "PullSocket 已关闭");
  }
}

}  // namespace voxorchestra::transport
