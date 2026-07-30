// SherpaAsrBackend 实现：封装 sherpa-onnx streaming zipformer C API。
//
// 调用序列与 sherpa_asr_smoke.cpp 一致（行为依据）：
//   CreateOnlineRecognizer → CreateOnlineStream →
//   OnlineStreamAcceptWaveform(float[-1,1], 16 kHz) → IsOnlineStreamReady /
//   DecodeOnlineStream 反复 → GetOnlineStreamResult 取文本；
//   尾部 0.3 s（4800 采样）静音补齐 + InputFinished 后做最终 decode。
// 会话边界由 IAsrBackend 的 is_last 控制（enable_endpoint=0，不切段）。
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "sherpa-onnx/c-api/c-api.h"

#include "voxorchestra/backend/sherpa_onnx/sherpa_asr_backend.hpp"

namespace voxorchestra::backend::sherpa_onnx {

namespace {

// 与冒烟参考相同的流式块：0.2 s（16000 Hz × 0.2 = 3200 采样）。
constexpr int kChunkSamples = 3200;
// 尾部静音补齐：0.3 s，InputFinished 前喂入（让尾部字音落定）。
constexpr int kTailPadSamples = 4800;

}  // namespace

struct SherpaAsrBackend::Impl {
  Impl(const std::string& model_dir, int num_threads) {
    const std::string enc = model_dir + "/encoder-epoch-99-avg-1.int8.onnx";
    const std::string dec = model_dir + "/decoder-epoch-99-avg-1.int8.onnx";
    const std::string jnr = model_dir + "/joiner-epoch-99-avg-1.int8.onnx";
    const std::string tok = model_dir + "/tokens.txt";

    SherpaOnnxOnlineTransducerModelConfig transducer;
    std::memset(&transducer, 0, sizeof(transducer));
    transducer.encoder = enc.c_str();
    transducer.decoder = dec.c_str();
    transducer.joiner = jnr.c_str();

    SherpaOnnxOnlineModelConfig mc;
    std::memset(&mc, 0, sizeof(mc));
    mc.tokens = tok.c_str();
    mc.num_threads = num_threads;
    mc.provider = "cpu";
    mc.debug = 0;
    mc.transducer = transducer;

    SherpaOnnxOnlineRecognizerConfig rc;
    std::memset(&rc, 0, sizeof(rc));
    rc.feat_config.sample_rate = kSampleRateHz;
    rc.feat_config.feature_dim = 80;
    rc.model_config = mc;
    rc.decoding_method = "greedy_search";
    rc.max_active_paths = 4;
    // 整段识别：不按端点切段（会话边界由 IAsrBackend 的 is_last 控制）。
    rc.enable_endpoint = 0;

    recognizer = SherpaOnnxCreateOnlineRecognizer(&rc);
    if (!recognizer) {
      throw std::runtime_error("sherpa-onnx 识别器创建失败: " + model_dir);
    }
  }

  ~Impl() {
    if (stream != nullptr) {
      SherpaOnnxDestroyOnlineStream(stream);
    }
    if (recognizer != nullptr) {
      SherpaOnnxDestroyOnlineRecognizer(recognizer);
    }
  }

  const SherpaOnnxOnlineRecognizer* recognizer = nullptr;
  const SherpaOnnxOnlineStream* stream = nullptr;  // 会话级，set_event_callback 重建
  EventCallback cb;
  std::atomic<bool> cancelled{false};
  std::vector<float> pending;  // 未满 0.2 s 块的归一化采样
  std::string last_partial;    // 上次已发出的 kPartial 文本（避免重复投递）
};

SherpaAsrBackend::SherpaAsrBackend(std::string model_dir, int num_threads) {
  impl_ = std::make_unique<Impl>(model_dir, num_threads);
}

SherpaAsrBackend::~SherpaAsrBackend() = default;

// 一次 AcceptWaveform → decode 至未就绪 → 取最新文本。
std::string SherpaAsrBackend::decode_and_get(const std::vector<float>& samples) {
  SherpaOnnxOnlineStreamAcceptWaveform(impl_->stream, kSampleRateHz,
                                       samples.data(), samples.size());
  while (SherpaOnnxIsOnlineStreamReady(impl_->recognizer, impl_->stream)) {
    SherpaOnnxDecodeOnlineStream(impl_->recognizer, impl_->stream);
  }
  const SherpaOnnxOnlineRecognizerResult* r =
      SherpaOnnxGetOnlineStreamResult(impl_->recognizer, impl_->stream);
  const std::string text = (r != nullptr) ? r->text : "";
  if (r != nullptr) {
    SherpaOnnxDestroyOnlineRecognizerResult(r);
  }
  return text;
}

void SherpaAsrBackend::set_event_callback(EventCallback cb) {
  impl_->cb = std::move(cb);
  impl_->cancelled.store(false);
  if (impl_->stream != nullptr) {
    SherpaOnnxDestroyOnlineStream(impl_->stream);
  }
  impl_->stream = SherpaOnnxCreateOnlineStream(impl_->recognizer);
  impl_->pending.clear();
  impl_->last_partial.clear();
}

void SherpaAsrBackend::feed_audio(const std::vector<int16_t>& pcm,
                                  bool is_last) {
  if (!impl_->cb || impl_->cancelled.load()) {
    return;
  }
  // int16 → float[-1,1]（sherpa AcceptWaveform 的输入约定）。
  for (const int16_t s : pcm) {
    impl_->pending.push_back(static_cast<float>(s) / 32768.0f);
  }
  while (impl_->pending.size() >= kChunkSamples) {
    if (impl_->cancelled.load()) {
      return;  // 协作式：块间检查取消，已置位则整次会话丢弃
    }
    std::vector<float> chunk(impl_->pending.begin(),
                             impl_->pending.begin() + kChunkSamples);
    impl_->pending.erase(impl_->pending.begin(),
                         impl_->pending.begin() + kChunkSamples);
    const std::string text = decode_and_get(chunk);
    if (!text.empty() && text != impl_->last_partial) {
      impl_->last_partial = text;
      impl_->cb({BackendEvent::Kind::kPartial, text, {}});
    }
  }
  if (is_last) {
    if (impl_->cancelled.load()) {
      return;
    }
    // 尾部补齐：剩余采样 + 0.3 s 静音，InputFinished 后最终 decode。
    std::vector<float> tail = std::move(impl_->pending);
    impl_->pending.clear();
    tail.resize(tail.size() + kTailPadSamples, 0.0f);
    decode_and_get(tail);
    SherpaOnnxOnlineStreamInputFinished(impl_->stream);
    while (SherpaOnnxIsOnlineStreamReady(impl_->recognizer, impl_->stream)) {
      SherpaOnnxDecodeOnlineStream(impl_->recognizer, impl_->stream);
    }
    const SherpaOnnxOnlineRecognizerResult* r =
        SherpaOnnxGetOnlineStreamResult(impl_->recognizer, impl_->stream);
    const std::string text = (r != nullptr) ? r->text : "";
    if (r != nullptr) {
      SherpaOnnxDestroyOnlineRecognizerResult(r);
    }
    if (!impl_->cancelled.load()) {
      impl_->cb({BackendEvent::Kind::kFinal, text, {}});
    }
  }
}

void SherpaAsrBackend::cancel() { impl_->cancelled.store(true); }

}  // namespace voxorchestra::backend::sherpa_onnx
