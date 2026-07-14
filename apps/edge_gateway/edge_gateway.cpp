#include "edge_gateway.hpp"

#include <utility>

#include "voxorchestra/protocol/message_envelope.hpp"

namespace voxorchestra::gateway {

using protocol::MessageEnvelope;
using protocol::MessageType;
using protocol::ProtocolError;

EdgeGateway::EdgeGateway(network::EventLoop* loop, const std::string& host,
                        std::uint16_t port)
   : loop_(loop), server_(loop, host, port) {
 server_.set_message_callback(
     [this](const std::shared_ptr<network::TcpConnection>& conn,
            const std::string& frame) { handle_message(conn, frame); });
 server_.set_protocol_error_callback(
     [this](const std::shared_ptr<network::TcpConnection>& conn,
            const std::string& message) { handle_protocol_error(conn, message); });
}

void EdgeGateway::start() { server_.start(); }

void EdgeGateway::stop() { server_.stop(); }

void EdgeGateway::handle_message(
   const std::shared_ptr<network::TcpConnection>& conn, const std::string& frame) {
 MessageEnvelope request;
 try {
   request = MessageEnvelope::from_json(frame);
 } catch (const ProtocolError& e) {
   // 非法请求：回结构化错误信封，连接保持可用（超长帧不会走到这里，
   // 由连接层直接关闭）。
   MessageEnvelope error_env;
   error_env.set_type(MessageType::kError);
   error_env.set_error({static_cast<int>(e.code()), e.what()});
   error_env.set_finish(true);
   conn->send(error_env.to_json() + "\n");
   return;
 }

 // 合法请求（起转发给 Unit Manager）：当前回 ack 信封占位。
 MessageEnvelope ack;
 ack.set_type(MessageType::kAck);
 ack.set_work_id(request.work_id());
 ack.set_request_id(request.request_id());
 ack.set_session_id(request.session_id());
 ack.set_payload({{"status", "ok"}});
 ack.set_finish(true);
 conn->send(ack.to_json() + "\n");
}

void EdgeGateway::handle_protocol_error(
   const std::shared_ptr<network::TcpConnection>& conn, const std::string& message) {
 // 连接级协议错误（超长帧）：客户端不可信，直接关闭，不回复。
 (void)conn;
 (void)message;
}

}  // namespace voxorchestra::gateway
