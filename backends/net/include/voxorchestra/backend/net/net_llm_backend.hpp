// NetLlmBackend：远端 llm_node 代理（流式 ILlmBackend 契约）。
//
// generate(prompt) 驱动一次节点推理：token 事件经数据面实时回放（pipeline
// 分句入队），done 收尾。负载 {"text": "<prompt>"}（与 llm_node 一致）。
#pragma once

#include <string>

#include <zmq.hpp>

#include "voxorchestra/backend/i_llm_backend.hpp"
#include "voxorchestra/backend/net/net_backend_session.hpp"

namespace voxorchestra::backend::net {

class NetLlmBackend final : public ILlmBackend {
 public:
  // 构造时同步 setup 节点（work_id 与节点任务一致）；节点不可达/超时
  // 抛异常 → 会话 setup 失败（调用方决定错误语义）。
  explicit NetLlmBackend(zmq::context_t& ctx, NetBackendConfig config)
      : session_(ctx, std::move(config)) {
    session_.setup();
  }

  void set_event_callback(EventCallback cb) override {
    session_.set_event_callback(std::move(cb));
  }

  void generate(const std::string& prompt) override {
    session_.drive_inference(session_.next_request_id("l"),
                             nlohmann::json{{"text", prompt}});
  }

  void cancel() override { session_.cancel(); }

 private:
  NetBackendSession session_;
};

}  // namespace voxorchestra::backend::net
