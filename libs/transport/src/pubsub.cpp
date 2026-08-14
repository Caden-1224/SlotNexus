#include "voxorchestra/transport/pubsub.hpp"

#include <cerrno>
#include <utility>

#include "voxorchestra/transport/transport_error.hpp"

namespace voxorchestra::transport {

namespace {

const char kReadyMarker[] = "READY";
const char kAckMarker[] = "ACK";

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

// ---------- PubSocket ----------

PubSocket::PubSocket(zmq::context_t& ctx) : ctx_(ctx) {}

PubSocket::~PubSocket() { close(); }

void PubSocket::bind(const std::string& data_endpoint) {
  throw_if_closed();
  data_endpoint_ = data_endpoint;
  pub_ = std::make_unique<zmq::socket_t>(ctx_, zmq::socket_type::pub);
  pub_->bind(data_endpoint_);
}

void PubSocket::bind_sync(const std::string& sync_endpoint) {
  throw_if_closed();
  sync_endpoint_ = sync_endpoint;
  sync_rep_ = std::make_unique<zmq::socket_t>(ctx_, zmq::socket_type::rep);
  sync_rep_->bind(sync_endpoint_);
}

void PubSocket::wait_subscriber_ready(std::chrono::milliseconds timeout) {
  throw_if_closed();
  if (!sync_rep_) {
    throw TransportError(TransportErrorCode::kRecvFailed,
                         "PubSocket 未 bind_sync");
  }
  sync_rep_->set(zmq::sockopt::rcvtimeo, static_cast<int>(timeout.count()));

  zmq::message_t msg;
  bool received = false;
  try {
    received = sync_rep_->recv(msg, zmq::recv_flags::none).has_value();
  } catch (zmq::error_t& e) {
    throw make_error(e);
  }
  if (!received) {
    throw TransportError(TransportErrorCode::kTimeout,
                         "等待订阅者就绪超时");
  }
  if (msg.to_string() != kReadyMarker) {
    throw TransportError(TransportErrorCode::kRecvFailed,
                         "订阅握手消息异常: " + msg.to_string());
  }
  try {
    sync_rep_->send(zmq::buffer(std::string(kAckMarker)), zmq::send_flags::none);
  } catch (zmq::error_t& e) {
    throw make_error(e);
  }
}

void PubSocket::publish(const std::string& topic, const std::string& payload) {
  throw_if_closed();
  if (!pub_) {
    throw TransportError(TransportErrorCode::kRecvFailed, "PubSocket 未 bind");
  }
  try {
    pub_->send(zmq::buffer(topic), zmq::send_flags::sndmore);
    pub_->send(zmq::buffer(payload), zmq::send_flags::none);
  } catch (zmq::error_t& e) {
    throw make_error(e);
  }
}

void PubSocket::close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  sync_rep_.reset();
  pub_.reset();
}

void PubSocket::throw_if_closed() const {
  if (closed_) {
    throw TransportError(TransportErrorCode::kClosed, "PubSocket 已关闭");
  }
}

// ---------- SubSocket ----------

SubSocket::SubSocket(zmq::context_t& ctx) : ctx_(ctx) {
  // socket 在构造时创建：订阅选项可以在 connect 之前设置，
  // 保证连接建立时订阅关系已经生效。
  sub_ = std::make_unique<zmq::socket_t>(ctx_, zmq::socket_type::sub);
  // 发布端先退出时不等待尚未发送完的订阅控制消息，避免 context 析构阻塞。
  sub_->set(zmq::sockopt::linger, 0);
}

SubSocket::~SubSocket() { close(); }

void SubSocket::subscribe(const std::string& prefix) {
  throw_if_closed();
  sub_->set(zmq::sockopt::subscribe, prefix);
}

void SubSocket::connect(const std::string& data_endpoint) {
  throw_if_closed();
  data_endpoint_ = data_endpoint;
  sub_->connect(data_endpoint_);
  connected_ = true;
}

void SubSocket::notify_ready(const std::string& sync_endpoint) {
  throw_if_closed();
  // 只发送 READY 不等待 ACK：握手只保证"发布前订阅已建立"，
  // 等待 ACK 会让单线程握手（发布端与订阅端同线程）互相阻塞。
  zmq::socket_t req(ctx_, zmq::socket_type::req);
  req.set(zmq::sockopt::sndtimeo, 3000);
  req.connect(sync_endpoint);

  try {
    req.send(zmq::buffer(std::string(kReadyMarker)), zmq::send_flags::none);
  } catch (zmq::error_t& e) {
    throw make_error(e);
  }
  // REQ socket 随作用域销毁；已入队的 READY 仍在管道中等待 REP 接收。
}

bool SubSocket::recv(std::string& topic, std::string& payload,
                     std::chrono::milliseconds timeout) {
  throw_if_closed();
  if (!sub_) {
    throw TransportError(TransportErrorCode::kRecvFailed,
                         "SubSocket 未 connect");
  }
  sub_->set(zmq::sockopt::rcvtimeo, static_cast<int>(timeout.count()));

  zmq::message_t topic_msg;
  bool got_topic = false;
  try {
    got_topic = sub_->recv(topic_msg, zmq::recv_flags::none).has_value();
  } catch (zmq::error_t& e) {
    throw make_error(e);
  }
  if (!got_topic) {
    return false;
  }

  zmq::message_t payload_msg;
  bool got_payload = false;
  try {
    got_payload = sub_->recv(payload_msg, zmq::recv_flags::none).has_value();
  } catch (zmq::error_t& e) {
    throw make_error(e);
  }
  if (!got_payload) {
    return false;
  }

  topic = topic_msg.to_string();
  payload = payload_msg.to_string();
  return true;
}

void SubSocket::close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  sub_.reset();
}

void SubSocket::throw_if_closed() const {
  if (closed_) {
    throw TransportError(TransportErrorCode::kClosed, "SubSocket 已关闭");
  }
}

}  // namespace voxorchestra::transport
