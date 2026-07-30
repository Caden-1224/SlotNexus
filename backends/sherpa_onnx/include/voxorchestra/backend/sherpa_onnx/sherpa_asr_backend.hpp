// SherpaAsrBackend：sherpa-onnx streaming zipformer 真实语音识别后端。
//
// 行为依据：artifacts/upstream-baseline/sherpa_asr_smoke.cpp（板端门禁已过）：
//   0.2 s 块流式喂入，greedy_search 整段识别，RTF ~1.07（4 threads）。
// 协议与 FakeAsrBackend 等价：
//   - feed_audio(帧, is_last=false) 按 0.2 s 块触发 decode，文本变化时
//     产出 kPartial（不逐 20 ms 帧重复投递相同文本）；
//   - is_last 帧处理后补齐 0.3 s 静音并 InputFinished，产出 kFinal 结束
//     会话（ASR 会话结束标记即 kFinal，与 Fake 一致，无 kDone）；
//   - cancel() 后 feed_audio 为空操作，不产出任何事件；
//   - set_event_callback 开启新会话：重建流并重置累计状态。
// 上游解码为同步阻塞；事件在 feed_audio 调用线程同步产出。模型目录与
// ONNX Runtime 线程数由构造注入（不硬编码）；int16 PCM 内部按 /32768.0f
// 归一化为 float（sherpa 的 AcceptWaveform 要求 float[-1,1]）。
//
// 上游源码不随仓库分发（third_party/README.md 约定），构建时由
// VOXORCHESTRA_SHERTA_ROOT 指向板端源码目录；模型文件不入库，
// 由配置传入（config/taishanpi3m/session.json::asr）。
#pragma once

#include <memory>
#include <string>

#include "voxorchestra/backend/i_asr_backend.hpp"

namespace voxorchestra::backend::sherpa_onnx {

class SherpaAsrBackend final : public IAsrBackend {
 public:
  // model_dir：模型目录（encoder/decoder/joiner 的 int8 onnx + tokens.txt）。
  // 加载失败抛出 std::runtime_error。
  // num_threads：ONNX Runtime 线程数（门禁基线 4，RTF ~1.07；线程数影响
  // 浮点归约顺序，最终识别文本以对应线程数的门禁基线为准）。
  SherpaAsrBackend(std::string model_dir, int num_threads = 4);
  ~SherpaAsrBackend() override;

  SherpaAsrBackend(const SherpaAsrBackend&) = delete;
  SherpaAsrBackend& operator=(const SherpaAsrBackend&) = delete;

  void set_event_callback(EventCallback cb) override;
  void feed_audio(const std::vector<int16_t>& pcm, bool is_last) override;
  void cancel() override;

 private:
  // 一次 AcceptWaveform → decode 至未就绪 → 取最新文本（.cpp 内调用）。
  std::string decode_and_get(const std::vector<float>& samples);

  struct Impl;  // 上游类型（SherpaOnnxOnlineRecognizer / Stream）只存在于 .cpp
  std::unique_ptr<Impl> impl_;
};

}  // namespace voxorchestra::backend::sherpa_onnx
