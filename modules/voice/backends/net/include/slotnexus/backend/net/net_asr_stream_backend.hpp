// NetAsrStreamBackend：流式 ASR 帧上行（会话侧）。
// Author: Caden
//
// 与 asr_node 的独立 PULL 通道配合：set_event_callback 开始时订阅本轮事件
// 主题并发送 start；feed_audio 每帧 base64 编码后 PUSH 上行，同时把 SUB
// 上的 partial/final 实时回放给调用方。节点侧仍复用 setup 建立的同一
// AsrNodeBackend，不再为每次 utterance 重新加载模型。
#pragma once

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <zmq.hpp>

#include "slotnexus/backend/i_asr_backend.hpp"
#include "slotnexus/backend/net/net_backend_session.hpp"
#include "slotnexus/common/base64.hpp"
#include "slotnexus/transport/pushpull.hpp"

namespace slotnexus::backend::net {

class NetAsrStreamBackend final : public IAsrBackend {
 public:
  explicit NetAsrStreamBackend(zmq::context_t& ctx, NetBackendConfig config)
      : session_(ctx, std::move(config)), push_(ctx) {
    const auto& cfg = session_.config();
    if (cfg.asr_stream_endpoint.empty()) {
      throw std::invalid_argument("NetAsrStreamBackend 缺少 asr_stream_endpoint");
    }
    push_.connect(cfg.asr_stream_endpoint);
    session_.setup();
  }

  void set_event_callback(EventCallback cb) override {
    session_.set_event_callback(std::move(cb));
    const auto& cfg = session_.config();
    request_id_ = session_.next_request_id("a");
    session_.begin_event_stream(request_id_);
    active_ = true;
    finished_ = false;
    send_json({{"op", "start"},
               {"work_id", cfg.work_id},
               {"request_id", request_id_}});
  }

  void feed_audio(const std::vector<std::int16_t>& pcm, bool is_last) override {
    if (!active_) {
      return;
    }
    const std::uint8_t* raw = reinterpret_cast<const std::uint8_t*>(pcm.data());
    nlohmann::json msg = {
        {"op", "frame"},
        {"work_id", session_.config().work_id},
        {"request_id", request_id_},
        {"last", is_last},
        {"pcm", common::base64_encode(raw, pcm.size() * sizeof(std::int16_t))}};
    send_json(msg);

    if (!is_last) {
      // 消费已到达的 partial，不阻塞生成/播放路径。
      session_.pump_event_stream(std::chrono::milliseconds(0), nullptr);
      return;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + session_.config().rpc_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      bool finished = false;
      session_.pump_event_stream(std::chrono::milliseconds(10), &finished);
      if (finished) {
        finished_ = true;
        active_ = false;
        return;
      }
    }
    active_ = false;
    throw std::runtime_error("流式 ASR final 超时");
  }

  void cancel() override {
    if (!active_) {
      return;
    }
    send_json({{"op", "cancel"},
               {"work_id", session_.config().work_id},
               {"request_id", request_id_}});
    active_ = false;
  }

 private:
  void send_json(const nlohmann::json& msg) {
    push_.send(msg.dump(), std::chrono::milliseconds(1000));
  }

  NetBackendSession session_;
  transport::PushSocket push_;
  std::string request_id_;
  bool active_ = false;
  bool finished_ = false;
};

}  // namespace slotnexus::backend::net
