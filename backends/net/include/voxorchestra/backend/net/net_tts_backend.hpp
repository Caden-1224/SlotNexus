// NetTtsBackend：远端 tts_node 代理（流式 ITtsBackend 契约）。
//
// synthesize(text) 驱动一次节点推理：PCM 帧事件经数据面实时回放（每帧
// 320 采样/20ms），done 收尾。负载 {"text": "<文本>"}（与 tts_node 一致）。
// 注意：同一 run 内多句合成（多条句子）时每次 synthesize 使用独立子流
// （t0/t1/...），事件按主题精确过滤，句子间不串扰。
#pragma once

#include <string>

#include <zmq.hpp>

#include "voxorchestra/backend/i_tts_backend.hpp"
#include "voxorchestra/backend/net/net_backend_session.hpp"

namespace voxorchestra::backend::net {

class NetTtsBackend final : public ITtsBackend {
 public:
  // 构造时同步 setup 节点（work_id 与节点任务一致）；节点不可达/超时
  // 抛异常 → 会话 setup 失败（调用方决定错误语义）。
  explicit NetTtsBackend(zmq::context_t& ctx, NetBackendConfig config)
      : session_(ctx, std::move(config)) {
    session_.setup();
  }

  void set_event_callback(EventCallback cb) override {
    session_.set_event_callback(std::move(cb));
  }

  void synthesize(const std::string& text) override {
    session_.drive_inference(session_.next_request_id("t"),
                             nlohmann::json{{"text", text}});
  }

  void cancel() override { session_.cancel(); }

 private:
  NetBackendSession session_;
};

}  // namespace voxorchestra::backend::net
