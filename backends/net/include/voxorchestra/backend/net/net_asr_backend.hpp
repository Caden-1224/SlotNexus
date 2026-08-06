// NetAsrBackend：远端 asr_node 代理（流式 IAsrBackend 契约）。
//
// 与本地 FakeAsrBackend 相同的同步契约：set_event_callback → feed_audio × N
// →（is_last）→ 帧内事件经数据面实时回放，kFinal 收尾。负载按配置切换：
//   - 帧数约定（asr_audio_uplink=false，默认）：{"text": "<帧数>"}，
//     节点 fake 后端按帧数合成确定性文本（Mock 回归基线）；
//   - 音频上行（asr_audio_uplink=true）：累积全部 PCM，is_last 时编码为
//     {"text": "pcm64:<base64>"} 上行，节点按真实后端（sherpa_onnx）
//     识别或按帧数约定解释（fake）。真机部署会话与节点可跨机，WAV
//     路径不通，音频上行不依赖共享文件系统。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <zmq.hpp>

#include "voxorchestra/backend/i_asr_backend.hpp"
#include "voxorchestra/backend/net/net_backend_session.hpp"
#include "voxorchestra/common/base64.hpp"

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
    frame_count_ = 0;   // 一次识别会话开始：重置帧计数与累积音频
    samples_.clear();
  }

  void feed_audio(const std::vector<int16_t>& pcm, bool is_last) override {
    ++frame_count_;
    if (config().asr_audio_uplink) {
      // 音频上行：累积样本，is_last 时整段编码上行（驱动线程独占）。
      samples_.insert(samples_.end(), pcm.begin(), pcm.end());
      if (!is_last) {
        return;
      }
      const std::uint8_t* raw = reinterpret_cast<const std::uint8_t*>(
          samples_.data());
      const std::string b64 =
          common::base64_encode(raw, samples_.size() * sizeof(int16_t));
      drive(std::string(voxorchestra::backend::kAsrPcmPayloadPrefix) + b64);
      return;
    }
    // 帧数约定：节点 fake 后端按帧数合成确定性识别文本。
    // is_last 未发生（会话无帧）时仍驱动一次（节点按 1 帧处理，与
    // run_mock 的防御语义一致）。
    if (is_last) {
      drive(std::to_string(frame_count_));
    }
  }

  void cancel() override { session_.cancel(); }

 private:
  const NetBackendConfig& config() const { return session_.config(); }

  void drive(const std::string& payload_text) {
    session_.drive_inference(session_.next_request_id("a"),
                             nlohmann::json{{"text", payload_text}});
    frame_count_ = 0;
    samples_.clear();
  }

  NetBackendSession session_;
  std::size_t frame_count_ = 0;
  std::vector<int16_t> samples_;  // 音频上行模式的累积样本
};

}  // namespace voxorchestra::backend::net
