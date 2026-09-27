// 通用数据面事件编解码实现。
//
// Author: Caden
// 入口：dataplane_event_to_payload/from_payload/to_envelope/from_envelope。
// 关键边界：kind 必须存在且 payload 必须为 JSON 对象；index/finish 随
// MessageEnvelope 保留，模块专有数据不在这里解释。
#include "slotnexus/dataplane/dataplane_event.hpp"

#include <utility>

namespace slotnexus::dataplane {

nlohmann::json dataplane_event_to_payload(const DataplaneEvent& e) {
  if (e.kind.empty()) {
    throw protocol::ProtocolError(protocol::ProtocolErrorCode::kMissingField,
                                  "数据面事件缺少 kind");
  }
  nlohmann::json j = e.payload;
  if (!j.is_object()) {
    throw protocol::ProtocolError(protocol::ProtocolErrorCode::kInvalidJson,
                                  "数据面事件 payload 必须为 JSON 对象");
  }
  j["kind"] = e.kind;
  return j;
}

DataplaneEvent dataplane_event_from_payload(const nlohmann::json& j) {
  if (!j.is_object()) {
    throw protocol::ProtocolError(protocol::ProtocolErrorCode::kInvalidJson,
                                  "数据面事件载荷必须为 JSON 对象");
  }
  if (!j.contains("kind") || !j["kind"].is_string()) {
    throw protocol::ProtocolError(protocol::ProtocolErrorCode::kMissingField,
                                  "事件载荷缺少 kind 字段");
  }
  DataplaneEvent e;
  e.kind = j["kind"].get<std::string>();
  e.payload = j;
  e.payload.erase("kind");
  return e;
}

protocol::MessageEnvelope dataplane_event_to_envelope(
    const DataplaneEvent& e, const std::string& work_id,
    const std::string& request_id) {
  protocol::MessageEnvelope env;
  env.set_type(protocol::MessageType::kEvent);
  env.set_work_id(work_id);
  env.set_request_id(request_id);
  env.set_index(e.index);
  env.set_finish(e.finish);
  env.set_payload(dataplane_event_to_payload(e));
  return env;
}

DataplaneEvent dataplane_event_from_envelope(
    const protocol::MessageEnvelope& env) {
  if (env.type() != protocol::MessageType::kEvent) {
    throw protocol::ProtocolError(
        protocol::ProtocolErrorCode::kInvalidType,
        "数据面消息类型必须为 event，实际为 " +
            protocol::message_type_to_string(env.type()));
  }
  DataplaneEvent e = dataplane_event_from_payload(env.payload());
  e.index = env.index();
  e.finish = env.finish();
  return e;
}

}  // namespace slotnexus::dataplane
