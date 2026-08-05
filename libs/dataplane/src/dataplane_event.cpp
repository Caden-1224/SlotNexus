// 数据面事件编解码实现：payload JSON 互转 + PCM 二进制 base64 编解码。
#include "voxorchestra/dataplane/dataplane_event.hpp"

#include <cstring>
#include <utility>

namespace voxorchestra::dataplane {

namespace {

// 最小 base64 编解码（RFC 4648 标准表，无外部依赖）。
constexpr char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::uint8_t* data, std::size_t n) {
  std::string out;
  out.reserve(((n + 2) / 3) * 4);
  for (std::size_t i = 0; i < n; i += 3) {
    const std::uint32_t b0 = data[i];
    const std::uint32_t b1 = (i + 1 < n) ? data[i + 1] : 0;
    const std::uint32_t b2 = (i + 2 < n) ? data[i + 2] : 0;
    const std::uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
    out.push_back(kBase64Chars[(triple >> 18) & 0x3F]);
    out.push_back(kBase64Chars[(triple >> 12) & 0x3F]);
    out.push_back(i + 1 < n ? kBase64Chars[(triple >> 6) & 0x3F] : '=');
    out.push_back(i + 2 < n ? kBase64Chars[triple & 0x3F] : '=');
  }
  return out;
}

int base64_char_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

std::vector<std::uint8_t> base64_decode(const std::string& s) {
  std::vector<std::uint8_t> out;
  out.reserve((s.size() / 4) * 3);
  int accum = 0;
  int bits = 0;
  for (const char c : s) {
    if (c == '=') {
      break;
    }
    const int v = base64_char_value(c);
    if (v < 0) {
      throw protocol::ProtocolError(protocol::ProtocolErrorCode::kInvalidJson,
                                    "非法 base64 字符");
    }
    accum = (accum << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((accum >> bits) & 0xFF));
    }
  }
  return out;
}

}  // namespace

nlohmann::json dataplane_event_to_payload(const DataplaneEvent& e) {
  nlohmann::json j;
  j["kind"] = e.kind;
  if (!e.text.empty()) {
    j["text"] = e.text;
  }
  if (!e.pcm.empty()) {
    j["pcm"] = base64_encode(e.pcm.data(), e.pcm.size());
  }
  return j;
}

DataplaneEvent dataplane_event_from_payload(const nlohmann::json& j) {
  DataplaneEvent e;
  if (!j.contains("kind") || !j["kind"].is_string()) {
    throw protocol::ProtocolError(protocol::ProtocolErrorCode::kMissingField,
                                  "事件载荷缺少 kind 字段");
  }
  e.kind = j["kind"].get<std::string>();
  if (j.contains("text") && j["text"].is_string()) {
    e.text = j["text"].get<std::string>();
  }
  if (j.contains("pcm") && j["pcm"].is_string()) {
    e.pcm = base64_decode(j["pcm"].get<std::string>());
  }
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

DataplaneEvent dataplane_event_from_backend(const backend::BackendEvent& e) {
  DataplaneEvent out;
  out.kind = backend::to_string(e.kind);
  out.text = e.text;
  // kFinal / kDone 是各自事件流的终止事件：映射为流尾标记（finish=true），
  // 消费者据此判断一条流结束（与消息信封的 finish 语义一致）。
  out.finish = (e.kind == backend::BackendEvent::Kind::kFinal ||
                e.kind == backend::BackendEvent::Kind::kDone);
  // int16 采样 → 字节（内存序，小端平台与 WAV/ALSA 一致）。
  out.pcm.resize(e.pcm.size() * sizeof(int16_t));
  std::memcpy(out.pcm.data(), e.pcm.data(), out.pcm.size());
  return out;
}

}  // namespace voxorchestra::dataplane
