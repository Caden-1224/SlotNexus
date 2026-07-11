// 统一消息信封（MessageEnvelope）。
//
// VoxOrchestra 控制面与数据面的所有消息都封装为同一结构，字段语义：
//   version      协议版本，当前为 1；不匹配即拒绝（版本链原则）
//   work_id      任务实例标识（Unit Manager 分配）
//   request_id   一次调用的标识，用于关联响应与日志
//   session_id   多轮会话的关联标识
//   type         消息类型（控制动作 / 数据事件），见 MessageType
//   index        流内序号，用于校验顺序与去重
//   timestamp_ms 发送方时间戳（UTC 毫秒）
//   payload      业务负载（JSON 对象，可为空）
//   finish       是否为该流的最后一条消息
//   error        结构化错误（code + message），无错误时为空
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace voxorchestra::protocol {

// 协议版本：与版本链中"协议"一环对应，修改消息格式时必须递增。
inline constexpr int kProtocolVersion = 1;

// 单条消息序列化后的最大字节数，防止超长消息占用内存与慢客户端拖垮服务。
inline constexpr std::size_t kMaxSerializedBytes = 1024 * 1024;  // 1 MiB

// 消息类型：控制动作（Setup/Inference/Cancel/TaskInfo/Exit）与数据事件。
enum class MessageType {
  kSetup,      // 创建任务实例
  kInference,  // 发起一次推理请求
  kCancel,     // 取消当前请求
  kTaskInfo,   // 查询任务状态
  kExit,       // 退出任务实例
  kEvent,      // 通用数据事件（audio/partial/final/token/pcm 由 payload.type 细分）
  kAck,        // 接收确认
  kError,      // 错误响应
};

// 消息类型字符串与枚举的互转；未知字符串返回 false / "unknown"。
bool message_type_from_string(const std::string& s, MessageType& out);
std::string message_type_to_string(MessageType t);

// 协议错误码。
enum class ProtocolErrorCode {
  kOk = 0,
  kInvalidJson,    // 不是合法 JSON
  kUnknownVersion, // 协议版本不匹配
  kInvalidType,    // 消息类型未知
  kMissingField,   // 缺少必需字段
  kOversized,      // 超过最大长度限制
};

// 协议错误：携带错误码的异常，供调用方决定返回哪个结构化错误。
class ProtocolError : public std::runtime_error {
 public:
  ProtocolError(ProtocolErrorCode code, const std::string& message)
      : std::runtime_error(message), code_(code) {}
  ProtocolErrorCode code() const { return code_; }

 private:
  ProtocolErrorCode code_;
};

// 结构化错误信息。
struct ErrorInfo {
  int code = 0;            // 错误码（0 表示无错误）
  std::string message;     // 人类可读的错误描述

  bool empty() const { return code == 0 && message.empty(); }
};

// 统一消息信封。
class MessageEnvelope {
 public:
  // 默认构造：版本为当前协议版本，其余字段为空。
  MessageEnvelope() = default;

  // ---- 字段访问器 ----
  int version() const { return version_; }
  void set_version(int v) { version_ = v; }

  const std::string& work_id() const { return work_id_; }
  void set_work_id(std::string v) { work_id_ = std::move(v); }

  const std::string& request_id() const { return request_id_; }
  void set_request_id(std::string v) { request_id_ = std::move(v); }

  const std::string& session_id() const { return session_id_; }
  void set_session_id(std::string v) { session_id_ = std::move(v); }

  MessageType type() const { return type_; }
  void set_type(MessageType t) { type_ = t; }

  int64_t index() const { return index_; }
  void set_index(int64_t v) { index_ = v; }

  int64_t timestamp_ms() const { return timestamp_ms_; }
  void set_timestamp_ms(int64_t v) { timestamp_ms_ = v; }

  const nlohmann::json& payload() const { return payload_; }
  nlohmann::json& payload() { return payload_; }
  void set_payload(nlohmann::json v) { payload_ = std::move(v); }

  bool finish() const { return finish_; }
  void set_finish(bool v) { finish_ = v; }

  const ErrorInfo& error() const { return error_; }
  void set_error(ErrorInfo v) { error_ = std::move(v); }

  // ---- 序列化 ----
  // 编码为 JSON 字符串；超过 kMaxSerializedBytes 抛 ProtocolError(kOversized)。
  std::string to_json() const;

  // 解码并校验；任何失败抛 ProtocolError：
  //   kInvalidJson / kUnknownVersion / kInvalidType / kMissingField / kOversized
  static MessageEnvelope from_json(const std::string& json);

 private:
  int version_ = kProtocolVersion;
  std::string work_id_;
  std::string request_id_;
  std::string session_id_;
  MessageType type_ = MessageType::kEvent;
  int64_t index_ = -1;
  int64_t timestamp_ms_ = -1;
  nlohmann::json payload_ = nlohmann::json::object();
  bool finish_ = false;
  ErrorInfo error_;
};

}  // namespace voxorchestra::protocol
