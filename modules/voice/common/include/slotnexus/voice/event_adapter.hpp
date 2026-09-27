// 语音事件适配器：voice BackendEvent ↔ 中间件通用事件。
//
// Author: Caden
// 设计意图：中间件只传输不透明 kind + JSON payload；语音专有的
// partial/token/done/PCM 语义、以及 PCM 字节到 base64 的编码都在本模块完成，
// 保持 core/dataplane 不出现音频类型。
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "slotnexus/backend/backend_event.hpp"
#include "slotnexus/common/base64.hpp"
#include "slotnexus/dataplane/dataplane_event.hpp"
#include "slotnexus/runtime/ibackend.hpp"

namespace slotnexus::voice {

inline slotnexus::runtime::BackendEvent ToRuntimeEvent(
    const slotnexus::backend::BackendEvent& e) {
  slotnexus::runtime::BackendEvent out;
  out.type = slotnexus::backend::to_string(e.kind);
  if (e.kind == slotnexus::backend::BackendEvent::Kind::kPcm) {
    const std::uint8_t* raw =
        reinterpret_cast<const std::uint8_t*>(e.pcm.data());
    out.payload = {{"pcm", slotnexus::common::base64_encode(
                                raw, e.pcm.size() * sizeof(std::int16_t))}};
  } else if (!e.text.empty()) {
    out.payload = {{"text", e.text}};
  }
  out.finish = (e.kind == slotnexus::backend::BackendEvent::Kind::kFinal ||
                e.kind == slotnexus::backend::BackendEvent::Kind::kDone);
  return out;
}

inline slotnexus::backend::BackendEvent FromDataplaneEvent(
    const slotnexus::dataplane::DataplaneEvent& e) {
  slotnexus::backend::BackendEvent out;
  if (e.kind == "partial") {
    out.kind = slotnexus::backend::BackendEvent::Kind::kPartial;
  } else if (e.kind == "final") {
    out.kind = slotnexus::backend::BackendEvent::Kind::kFinal;
  } else if (e.kind == "token") {
    out.kind = slotnexus::backend::BackendEvent::Kind::kToken;
  } else if (e.kind == "pcm") {
    out.kind = slotnexus::backend::BackendEvent::Kind::kPcm;
  } else if (e.kind == "done") {
    out.kind = slotnexus::backend::BackendEvent::Kind::kDone;
  } else {
    throw std::runtime_error("语音数据面事件类型未知: " + e.kind);
  }
  if (e.payload.is_object() && e.payload.contains("text") &&
      e.payload["text"].is_string()) {
    out.text = e.payload["text"].get<std::string>();
  }
  if (e.payload.is_object() && e.payload.contains("pcm") &&
      e.payload["pcm"].is_string()) {
    const std::vector<std::uint8_t> bytes = slotnexus::common::base64_decode(
        e.payload["pcm"].get<std::string>());
    out.pcm.resize(bytes.size() / sizeof(std::int16_t));
    if (!bytes.empty()) {
      std::memcpy(out.pcm.data(), bytes.data(), bytes.size());
    }
  }
  return out;
}

}  // namespace slotnexus::voice
