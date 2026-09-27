// 通用数据面事件（DataplaneEvent）：应用自定义 kind + 不透明 JSON payload。
// Author: Caden
//
// 设计意图：中间件只提供流式事件传输与顺序/完成标记，不内置 partial、
// token、PCM 等语音语义，也不做音频字节编解码。具体模块在适配器中决定
// payload 的形状；语音模块当前把文本事件放在 payload.text，把 PCM 帧以
// base64 放在 payload.pcm（兼容旧数据面线格式）。
//
// 主要逻辑：payload JSON 与 MessageEnvelope（type=kEvent）互转；index 与
// finish 使用信封级字段，保持与既有消息协议兼容。
#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "slotnexus/protocol/message_envelope.hpp"

namespace slotnexus::dataplane {

// 单条通用数据面事件。kind 为应用定义字符串；payload 必须为 JSON 对象。
struct DataplaneEvent {
  std::string kind;
  nlohmann::json payload = nlohmann::json::object();
  int64_t index = -1;
  bool finish = false;
};

// 载荷部分（payload JSON）与完整信封（type=kEvent）互转。
// 解析失败（缺 kind、payload 非对象）抛 ProtocolError。
nlohmann::json dataplane_event_to_payload(const DataplaneEvent& e);
DataplaneEvent dataplane_event_from_payload(const nlohmann::json& j);

protocol::MessageEnvelope dataplane_event_to_envelope(
    const DataplaneEvent& e, const std::string& work_id,
    const std::string& request_id);
DataplaneEvent dataplane_event_from_envelope(
    const protocol::MessageEnvelope& env);

}  // namespace slotnexus::dataplane
