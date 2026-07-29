// SummerTtsBackend：SummerTTS vits（仅 Eigen，无外部 NN 运行时）真实语音合成后端。
//
// 行为依据：artifacts/upstream-baseline/tts_smoke.cpp（板端门禁已过）：
//   16 kHz mono S16 PCM，RTF ~0.62，峰值 RSS 407.9 MB。
// 协议与 FakeTtsBackend 等价：
//   - synthesize(text) 产出 kPcm…（每帧 ≤ kFrameSamples=320 采样，20 ms），
//     全部音频产出后 kDone；
//   - cancel() 后 synthesize 为空操作，不产出任何事件；
//   - set_event_callback 开启新会话：重置取消状态。
// 上游 infer 为同步阻塞（整句合成，门禁基线 ~2 s）；事件在 synthesize
// 调用线程同步产出。模型路径 / 语速倍率由构造注入（不硬编码），
// sid 固定 0（single_speaker 模型）。
//
// 上游源码不随仓库分发（third_party/README.md 约定），构建时由
// VOXORCHESTRA_SUMMERTTS_ROOT 指向板端源码目录；模型文件不入库，
// 由配置传入（config/taishanpi3m/session.json::tts）。
#pragma once

#include <memory>
#include <string>

#include "voxorchestra/backend/i_tts_backend.hpp"

namespace voxorchestra::backend::summer_tts {

class SummerTtsBackend final : public ITtsBackend {
 public:
  // model_path：single_speaker_fast.bin；加载失败抛出 std::runtime_error。
  // length_scale：语速倍率，1.0 为门禁基线原速。
  SummerTtsBackend(std::string model_path, float length_scale = 1.0f);
  ~SummerTtsBackend() override;

  SummerTtsBackend(const SummerTtsBackend&) = delete;
  SummerTtsBackend& operator=(const SummerTtsBackend&) = delete;

  void set_event_callback(EventCallback cb) override;
  void synthesize(const std::string& text) override;
  void cancel() override;

 private:
  struct Impl;  // 上游类型（SynthesizerTrn / 模型缓冲）只存在于 .cpp
  std::unique_ptr<Impl> impl_;
};

}  // namespace voxorchestra::backend::summer_tts
