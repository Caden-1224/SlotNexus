// NetAsrBackend：远端 asr_node 代理（流式 IAsrBackend 契约）。
//
// 与本地 FakeAsrBackend 相同的同步契约：set_event_callback → feed_audio × N
// →（is_last）→ 帧内事件经数据面实时回放，kFinal 收尾。负载按帧数约定
// （{"text": "<帧数>"}，与 fake 节点一致）：会话侧累积帧数，is_last 时
// 驱动一次节点推理。真实后端（sherpa_onnx）按 WAV 路径驱动，真机部署时
// 切换到路径模式（见 session_node 的 net 模式文档）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <zmq.hpp>

#include "voxorchestra/backend/i_asr_backend.hpp"
#include "voxorchestra/backend/net/net_backend_session.hpp"

namespace voxorchestra::backend::net {

class NetAsrBackend final : public IAsrBackend {
 public:
  // 构造时同步 setup 节点（work_id 与节点任务一致）；节点不可达/超时
  // 抛异常 → 会话 setup 失败（调用方决定错误语义）。
  explicit NetAsrBackend(zmq::context_t& ctx, NetBackendConfig config)
      : session_(ctx, std::move(config)) {
    session_.setup();
  }

  void set_event_callback(EventCallback cb) override {
    session_.set_event_callback(std::move(cb));
    frame_count_ = 0;  // 一次识别会话开始：重置帧计数
  }

  void feed_audio(const std::vector<int16_t>& pcm, bool is_last) override {
    ++frame_count_;
    if (!is_last) {
      return;
    }
    // 帧数约定：节点 fake 后端按帧数合成确定性识别文本。
    // is_last 未发生（会话无帧）时仍驱动一次（节点按 1 帧处理，与
    // run_mock 的防御语义一致）。
    session_.drive_inference(
        session_.next_request_id("a"),
        nlohmann::json{{"text", std::to_string(frame_count_)}});
    frame_count_ = 0;
  }

  void cancel() override { session_.cancel(); }

 private:
  NetBackendSession session_;
  std::size_t frame_count_ = 0;
};

}  // namespace voxorchestra::backend::net
