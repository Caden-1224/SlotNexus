// SummerTtsBackend 实现：封装 SummerTTS vits 推理（ttsLoadModel →
// SynthesizerTrn → infer），调用序列与 tts_smoke.cpp 一致：
//   ttsLoadModel(model_path, &model_data) 返回模型大小
//   SynthesizerTrn(model_data, size) 持有模型缓冲引用
//   infer(text, sid=0, length_scale, data_len) 返回 int16_t*（tts_free_data 释放）
//
// 板端编译注意（与 CMakeLists.smoke.txt 相同，见本目录 CMakeLists 注释）：
//   - 上游若干头用 uint16_t 未自给自足 <cstdint>：-include cstdint 补齐；
//   - 作者 CMakeLists 漏入 engipa/ipa.cpp：本库已补齐该编译单元。
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "SynthesizerTrn.h"
#include "utils.h"

#include "voxorchestra/backend/summer_tts/summer_tts_backend.hpp"

namespace voxorchestra::backend::summer_tts {

struct SummerTtsBackend::Impl {
  Impl(float* model_data, int32_t model_size, float scale)
      : model_data(model_data), length_scale(scale), synth(model_data, model_size) {}

  ~Impl() {
    // SynthesizerTrn 引用 model_data 缓冲；成员按声明逆序析构，
    // synth 先于 model_data 销毁，随后才释放模型缓冲。
    if (model_data != nullptr) {
      tts_free_data(model_data);
    }
  }

  float* model_data;      // ttsLoadModel 产出，tts_free_data 释放
  float length_scale;     // 语速倍率（1.0 = 门禁基线原速）
  SynthesizerTrn synth;   // 持有 model_data 引用，整句推理入口
  EventCallback cb;
  std::atomic<bool> cancelled{false};
};

SummerTtsBackend::SummerTtsBackend(std::string model_path, float length_scale) {
  float* model_data = nullptr;
  const int32_t model_size = ttsLoadModel(model_path.data(), &model_data);
  if (model_size <= 0 || model_data == nullptr) {
    throw std::runtime_error("SummerTTS 模型加载失败: " + model_path);
  }
  impl_ = std::make_unique<Impl>(model_data, model_size, length_scale);
}

SummerTtsBackend::~SummerTtsBackend() = default;

void SummerTtsBackend::set_event_callback(EventCallback cb) {
  impl_->cb = std::move(cb);
  impl_->cancelled.store(false);
}

void SummerTtsBackend::synthesize(const std::string& text) {
  if (!impl_->cb || impl_->cancelled.load()) {
    return;
  }
  int32_t data_len = 0;
  int16_t* wav = impl_->synth.infer(text, /*sid=*/0, impl_->length_scale, data_len);
  if (wav == nullptr || data_len <= 0) {
    return;  // 上游合成失败：不产出任何事件（协议无错误事件）
  }
  // infer 阻塞期间 cancel 可被置位：已取消则整次结果丢弃，不产出事件。
  if (!impl_->cancelled.load()) {
    // 按 20 ms 帧（320 采样）切分投递；data_len 余数在最后一帧原样发出
    // （协议层对帧长无断言，FakeAudioSink / session 按整块拼接）。
    std::size_t offset = 0;
    const std::size_t total = static_cast<std::size_t>(data_len);
    while (offset < total) {
      const std::size_t n =
          std::min<std::size_t>(static_cast<std::size_t>(kFrameSamples), total - offset);
      std::vector<int16_t> pcm(wav + offset, wav + offset + n);
      offset += n;
      impl_->cb({BackendEvent::Kind::kPcm, {}, std::move(pcm)});
    }
    impl_->cb({BackendEvent::Kind::kDone, {}, {}});
  }
  tts_free_data(wav);
}

void SummerTtsBackend::cancel() { impl_->cancelled.store(true); }

}  // namespace voxorchestra::backend::summer_tts
