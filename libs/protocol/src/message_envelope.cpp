#include "voxorchestra/protocol/message_envelope.hpp"

#include <array>
#include <stdexcept>
#include <utility>

namespace voxorchestra::protocol {

namespace {

const std::array<std::pair<MessageType, const char*>, 8>& kTypeNames() {
  static const std::array<std::pair<MessageType, const char*>, 8> kNames{{
      {MessageType::kSetup, "setup"},
      {MessageType::kInference, "inference"},
      {MessageType::kCancel, "cancel"},
      {MessageType::kTaskInfo, "taskinfo"},
      {MessageType::kExit, "exit"},
      {MessageType::kEvent, "event"},
      {MessageType::kAck, "ack"},
      {MessageType::kError, "error"},
  }};
  return kNames;
}

}  // namespace

bool message_type_from_string(const std::string& s, MessageType& out) {
  for (const auto& [type, name] : kTypeNames()) {
    if (s == name) {
      out = type;
      return true;
    }
  }
  return false;
}

std::string message_type_to_string(MessageType t) {
  for (const auto& [type, name] : kTypeNames()) {
    if (type == t) {
      return name;
    }
  }
  return "unknown";
}

std::string MessageEnvelope::to_json() const {
  nlohmann::json obj = {
      {"version", version_},
      {"work_id", work_id_},
      {"request_id", request_id_},
      {"session_id", session_id_},
      {"type", message_type_to_string(type_)},
      {"index", index_},
      {"timestamp_ms", timestamp_ms_},
      {"payload", payload_},
      {"finish", finish_},
      {"error",
       error_.empty()
           ? nlohmann::json::object()
           : nlohmann::json{{"code", error_.code}, {"message", error_.message}}},
  };
  // replace 错误处理：串行化对任意内容字节总成功。解析诊断/客户端字段
  // 可能携带非 UTF-8 字节（如非法输入被内嵌进错误信息），默认严格校验
  // 会在 dump 抛 type_error.316，未捕获时直接终止进程——非法输入必须
  // 回结构化错误而不是崩掉网关/节点（非法 JSON 故障注入回归发现）。
  const std::string s =
      obj.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  if (s.size() > kMaxSerializedBytes) {
    throw ProtocolError(ProtocolErrorCode::kOversized,
                        "序列化后的消息超过 " + std::to_string(kMaxSerializedBytes) + " 字节");
  }
  return s;
}

MessageEnvelope MessageEnvelope::from_json(const std::string& json) {
  if (json.size() > kMaxSerializedBytes) {
    throw ProtocolError(ProtocolErrorCode::kOversized,
                        "输入消息超过 " + std::to_string(kMaxSerializedBytes) + " 字节");
  }

  nlohmann::json obj;
  try {
    obj = nlohmann::json::parse(json);
  } catch (const nlohmann::json::exception& e) {
    throw ProtocolError(ProtocolErrorCode::kInvalidJson, std::string("JSON 解析失败: ") + e.what());
  }
  if (!obj.is_object()) {
    throw ProtocolError(ProtocolErrorCode::kInvalidJson, "消息必须是 JSON 对象");
  }

  MessageEnvelope env;
  try {
    // 所有类型化字段访问都可能因类型错误抛出 nlohmann 异常，统一转为
    // ProtocolError，避免原始异常逃逸到网关/节点进程导致崩溃。
    const int version = obj.value("version", 0);
    if (version != kProtocolVersion) {
      throw ProtocolError(ProtocolErrorCode::kUnknownVersion,
                          "协议版本 " + std::to_string(version) + " 不受支持，当前版本为 " +
                              std::to_string(kProtocolVersion));
    }

    if (!obj.contains("type")) {
      throw ProtocolError(ProtocolErrorCode::kMissingField, "缺少必需字段 type");
    }
    MessageType type{};
    if (!message_type_from_string(obj["type"].get<std::string>(), type)) {
      throw ProtocolError(ProtocolErrorCode::kInvalidType,
                          "未知消息类型: " + obj["type"].get<std::string>());
    }

    env.set_version(version);
    env.set_type(type);
    env.set_work_id(obj.value("work_id", std::string{}));
    env.set_request_id(obj.value("request_id", std::string{}));
    env.set_session_id(obj.value("session_id", std::string{}));
    env.set_index(obj.value("index", int64_t{-1}));
    env.set_timestamp_ms(obj.value("timestamp_ms", int64_t{-1}));
    env.set_finish(obj.value("finish", false));
    if (obj.contains("payload")) {
      env.set_payload(obj["payload"]);
    }
    if (obj.contains("error") && obj["error"].is_object()) {
      ErrorInfo e;
      e.code = obj["error"].value("code", 0);
      e.message = obj["error"].value("message", std::string{});
      env.set_error(std::move(e));
    }
  } catch (const nlohmann::json::exception& e) {
    throw ProtocolError(ProtocolErrorCode::kInvalidJson,
                        std::string("消息字段类型错误: ") + e.what());
  }
  return env;
}

}  // namespace voxorchestra::protocol
