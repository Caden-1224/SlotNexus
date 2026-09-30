// 通用控制面响应助手。
//
// Author: Caden
// 设计意图：只负责回显 work_id/request_id/session_id 并封装 ack/error，
// 不对业务负载做字段解释。它被 Gateway、Manager 和 Node Host 共用，
// 因此不归属于任何一个具体服务目录。
#pragma once

#include <string>

#include "slotnexus/protocol/message_envelope.hpp"

namespace slotnexus::app {

inline protocol::MessageEnvelope BuildAck(const protocol::MessageEnvelope& request,
                                          nlohmann::json payload) {
  protocol::MessageEnvelope reply;
  reply.set_type(protocol::MessageType::kAck);
  reply.set_work_id(request.work_id());
  reply.set_request_id(request.request_id());
  reply.set_session_id(request.session_id());
  reply.set_payload(std::move(payload));
  reply.set_finish(true);
  return reply;
}

inline protocol::MessageEnvelope BuildError(const protocol::MessageEnvelope& request,
                                            int code, const std::string& message) {
  protocol::MessageEnvelope reply;
  reply.set_type(protocol::MessageType::kError);
  reply.set_work_id(request.work_id());
  reply.set_request_id(request.request_id());
  reply.set_session_id(request.session_id());
  reply.set_error({code, message});
  reply.set_finish(true);
  return reply;
}

}  // namespace slotnexus::app
