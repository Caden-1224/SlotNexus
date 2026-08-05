// 数据面事件（DataplaneEvent）：流式后端中间事件的统一载体。
//
// 沿数据面 PUB/SUB 通道从节点流向会话，承载 ASR partial/final、LLM
// token/done、TTS PCM 帧。事件本体不带标识（work_id/request_id 等）——
// 与 backends/backend_event.hpp 的约定一致，标识是节点外壳/会话的职责：
// 发布时由 EventPublisher 填入信封的 work_id/request_id 字段。
//
// 消息形态：统一信封（MessageEnvelope，type=kEvent），index/finish 走
// 信封级字段（与消息协议语义一致），payload 只放业务内容：
//   {"kind": "partial|final|token|done|pcm", "text": "...", "pcm": "<base64>"}
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "voxorchestra/backend/backend_event.hpp"
#include "voxorchestra/protocol/message_envelope.hpp"

namespace voxorchestra::dataplane {

// kind 取值：与 backends/backend_event.hpp 的 BackendEvent::Kind 一一对应，
// 节点适配器负责把后端事件转换为数据面事件。
inline constexpr const char* kKindPartial = "partial";  // ASR 中间结果
inline constexpr const char* kKindFinal = "final";      // ASR 最终文本
inline constexpr const char* kKindToken = "token";      // LLM 增量 token
inline constexpr const char* kKindDone = "done";        // LLM 生成完成
inline constexpr const char* kKindPcm = "pcm";          // TTS PCM 帧

// 单条事件。pcm 为原始字节（16kHz/单声道/16bit，20ms 帧 320 字节），
// 网络编码时转 base64；index 为流内序号（校验顺序），finish 标记流尾。
struct DataplaneEvent {
  std::string kind;
  std::string text;                             // 文本类事件内容（pcm 事件为空）
  std::vector<std::uint8_t> pcm;                // PCM 帧二进制（kind==kKindPcm）
  int64_t index = -1;
  bool finish = false;
};

// ---- 编解码 ----
// 载荷部分（payload JSON）与完整信封（type=kEvent）互转。
// 解析失败（类型不符/缺字段/非法 base64）抛 ProtocolError。
nlohmann::json dataplane_event_to_payload(const DataplaneEvent& e);
DataplaneEvent dataplane_event_from_payload(const nlohmann::json& j);

protocol::MessageEnvelope dataplane_event_to_envelope(
    const DataplaneEvent& e, const std::string& work_id,
    const std::string& request_id);
DataplaneEvent dataplane_event_from_envelope(
    const protocol::MessageEnvelope& env);

// 后端事件 → 数据面事件：kind 走 backend::to_string（"partial" 等，与
// kKind 常量一致），PCM int16 采样按内存字节序转为字节（小端平台与
// WAV/ALSA 约定一致）。供节点适配器把流式后端事件转出数据面。
DataplaneEvent dataplane_event_from_backend(const backend::BackendEvent& e);

}  // namespace voxorchestra::dataplane
