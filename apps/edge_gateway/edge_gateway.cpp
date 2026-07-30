#include "edge_gateway.hpp"

#include <utility>

#include "voxorchestra/common/log.hpp"
#include "voxorchestra/protocol/message_envelope.hpp"
#include "voxorchestra/transport/transport_error.hpp"

namespace voxorchestra::gateway {

using protocol::MessageEnvelope;
using protocol::MessageType;
using protocol::ProtocolError;
using protocol::ProtocolErrorCode;

EdgeGateway::EdgeGateway(network::EventLoop* loop, const std::string& host,
                         std::uint16_t port, const std::string& manager_endpoint,
                         std::chrono::milliseconds forward_deadline)
    : loop_(loop), server_(loop, host, port),
      manager_endpoint_(manager_endpoint), forward_deadline_(forward_deadline) {
  server_.set_message_callback(
      [this](const std::shared_ptr<network::TcpConnection>& conn,
             const std::string& frame) { handle_message(conn, frame); });
  server_.set_protocol_error_callback(
      [this](const std::shared_ptr<network::TcpConnection>& conn,
             const std::string& message) { handle_protocol_error(conn, message); });
}

EdgeGateway::~EdgeGateway() {
  if (ctx_ != nullptr) {
    // stop() 未能在 loop 线程销毁（事件循环已退出，进程即将结束）：
    // 放弃显式 term，由操作系统回收。zmq_ctx_term 在 REQ socket 停留
    // 于死端点重连轮询时可能永久阻塞，进程退出不该卡在清理上。
    (void)ctx_.release();
  }
}

void EdgeGateway::start() {
  // start() 由调用方经 run_in_loop 在 loop 线程执行：zmq 对象与
  // 使用它们的线程（loop 线程）保持一致。
  ctx_ = std::make_unique<zmq::context_t>(1);
  manager_ = std::make_unique<transport::RpcClient>(*ctx_);
  manager_->connect(manager_endpoint_);
  server_.start();
}

void EdgeGateway::stop() {
  if (loop_->running()) {
    // 在 loop 线程销毁 zmq 对象（创建/使用/销毁同线程，zmq 不允许跨
    // 线程操作 socket）。事件循环已退出时跳过：进程即将结束，由析构
    // 释放 context，操作系统回收。
    loop_->run_in_loop([this] {
      manager_.reset();
      ctx_.reset();
    });
  }
  server_.stop();
}

void EdgeGateway::handle_message(
    const std::shared_ptr<network::TcpConnection>& conn, const std::string& frame) {
  MessageEnvelope request;
  try {
    request = MessageEnvelope::from_json(frame);
  } catch (const ProtocolError& e) {
    // 非法请求：回结构化错误信封，连接保持可用（超长帧不会走到这里，
    // 由连接层直接关闭）。
    common::LogLine("gw err bad_json frame=" + frame.substr(0, 80));
    MessageEnvelope error_env;
    error_env.set_type(MessageType::kError);
    error_env.set_error({static_cast<int>(e.code()), e.what()});
    error_env.set_finish(true);
    conn->send(error_env.to_json() + "\n");
    return;
  }
  // 请求级日志：request_id 在此进入网关日志（门禁 3）。
  common::LogLine("gw req request_id=" + request.request_id() + " type=" +
                  protocol::message_type_to_string(request.type()) +
                  " work_id=" + request.work_id());

  switch (request.type()) {
    case MessageType::kSetup:
    case MessageType::kInference:
    case MessageType::kCancel:
    case MessageType::kTaskInfo:
    case MessageType::kExit:
      // 合法 action：转发给 Unit Manager，把响应送回原连接。
      forward_to_manager(conn, frame);
      return;
    default:
      // ack/event/error 是服务端→客户端方向，客户端不可发送。
      common::LogLine("gw err request_id=" + request.request_id() +
                      " invalid_client_type");
      MessageEnvelope reject;
      reject.set_type(MessageType::kError);
      reject.set_work_id(request.work_id());
      reject.set_request_id(request.request_id());
      reject.set_session_id(request.session_id());
      reject.set_error({static_cast<int>(ProtocolErrorCode::kInvalidType),
                        "客户端不允许发送该类型: " +
                            protocol::message_type_to_string(request.type())});
      reject.set_finish(true);
      conn->send(reject.to_json() + "\n");
      return;
  }
}

void EdgeGateway::forward_to_manager(
    const std::shared_ptr<network::TcpConnection>& conn, const std::string& frame) {
  // 解析请求信封用于日志关联（失败时为空信封，request_id 为空串）。
  MessageEnvelope request;
  try {
    request = MessageEnvelope::from_json(frame);
  } catch (const ProtocolError&) {
    request = MessageEnvelope{};
  }
  try {
    const std::string reply = manager_->call(frame, forward_deadline_);
    MessageEnvelope reply_env;
    try {
      reply_env = MessageEnvelope::from_json(reply);
    } catch (const ProtocolError&) {
      // 管理器回复异常不在此处理：原样转发，由客户端与测试暴露。
    }
    common::LogLine("gw reply request_id=" + request.request_id() + " type=" +
                    protocol::message_type_to_string(reply_env.type()));
    conn->send(reply + "\n");
  } catch (const transport::TransportError& e) {
    // Manager 不可达或超时：回错误信封，连接保持可用，客户端可重试。
    common::LogLine("gw err request_id=" + request.request_id() +
                    " manager_unreachable");
    MessageEnvelope error_env;
    error_env.set_type(MessageType::kError);
    error_env.set_work_id(request.work_id());
    error_env.set_request_id(request.request_id());
    error_env.set_session_id(request.session_id());
    error_env.set_error({-1, "manager_unreachable: " + std::string(e.what())});
    error_env.set_finish(true);
    conn->send(error_env.to_json() + "\n");
  }
}

void EdgeGateway::handle_protocol_error(
    const std::shared_ptr<network::TcpConnection>& conn, const std::string& message) {
  // 连接级协议错误（超长帧）：客户端不可信，直接关闭，不回复。
  (void)conn;
  (void)message;
}

}  // namespace voxorchestra::gateway
