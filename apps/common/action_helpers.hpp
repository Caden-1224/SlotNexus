// 控制面共享助手：响应信封构造与负载约定。
//
// 负载约定（客户端 ↔ 网关 ↔ Manager ↔ 节点 全链路一致）：
//   inference 请求 payload = {"text": "..."}
//   成功响应 payload  = {"text": 结果} / {"status": "ok"} / {"state": ...}
//   失败响应 type = error，error = {code: 运行时错误码, message: 错误名}
#pragma once

#include <string>

#include "voxorchestra/protocol/message_envelope.hpp"

namespace voxorchestra::app {

// ack 信封：回显请求的 work_id / request_id / session_id。
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

// error 信封：回显关联字段，error = {code, message}。
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

// 从推理请求 payload 提取文本（{"text": "..."}）；缺失或类型不符返回空串。
inline std::string ExtractText(const nlohmann::json& payload) {
  const auto it = payload.find("text");
  return (it != payload.end() && it->is_string()) ? it->get<std::string>() : "";
}

}  // namespace voxorchestra::app
