// 后端统一事件模型。
//
// ASR（partial/final）、LLM（token/done）、TTS（pcm/done）的产出全部统一为
// 一种 BackendEvent 结构，由各 Backend 接口通过 EventCallback 投递：
//   - kPartial / kFinal   流式识别的中间/最终文本（IAsrBackend）
//   - kToken              流式生成的单个 token 文本（ILlmBackend）
//   - kPcm                一帧 16-bit 单声道音频（ITtsBackend 产出，IAudioSink 消费）
//   - kDone               一次会话/一次生成的正常结束标记
//
// 标识符（work_id/request_id/session_id/generation）不属于后端事件：它们是
// Node 外壳与 Session 编排层的职责，后端只产出纯业务数据。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace voxorchestra::backend {

// 统一后端事件。
struct BackendEvent {
  enum class Kind { kPartial, kFinal, kToken, kPcm, kDone };

  Kind kind = Kind::kDone;
  std::string text;            // kPartial / kFinal / kToken 的文本
  std::vector<int16_t> pcm;    // kPcm 的音频帧（16-bit 单声道）
};

// 事件回调：Backend 每次产出事件时同步或异步调用；取消后不得再调用。
using EventCallback = std::function<void(const BackendEvent&)>;

// 事件类型字符串化（日志与调试用）。
inline const char* to_string(BackendEvent::Kind kind) {
  switch (kind) {
    case BackendEvent::Kind::kPartial: return "partial";
    case BackendEvent::Kind::kFinal:   return "final";
    case BackendEvent::Kind::kToken:   return "token";
    case BackendEvent::Kind::kPcm:     return "pcm";
    case BackendEvent::Kind::kDone:    return "done";
  }
  return "unknown";
}

// 音频格式常量：全项目统一为 16 kHz 单声道 16-bit PCM。
inline constexpr int kSampleRateHz = 16000;
inline constexpr int kChannels = 1;
inline constexpr int kFrameSamples = 320;  // 20 ms 帧（16000 * 0.02）

}  // namespace voxorchestra::backend
