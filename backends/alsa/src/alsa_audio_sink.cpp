// AlsaAudioSink 实现：板端 ALSA 播放。
//
// 参数设置与错误处理对齐上游 sherpa-onnx AlsaPlay（板端
// ~/workspace/upstream_rkllm/sherpa-onnx/sherpa-onnx/csrc/alsa-play.cc，
// 开发机同源副本见主机参考目录 开源参考/LLM_Voice_Flow/）：
//   - 阻塞模式 snd_pcm_open（不置 SND_PCM_NONBLOCK，直接 writei 无 -EAGAIN
//     处理，照上游）；
//   - ACCESS_RW_INTERLEAVED / S16_LE / 单声道 / rate_near；
//   - 不强制 period size：取 hw 默认 period，按它分块 writei（上游同款）；
//   - -EPIPE（underrun/XRUN）→ snd_pcm_prepare 恢复后重试该块一次；
//   - close 前 snd_pcm_drain（等全部播完，上游 Drain() 语义照抄）。
//
// 与上游的差异：上游 AlsaPlay 在采样率与硬件不一致时内部建带限 sinc
// LinearResample 重采样；本类用候选率阶梯（requested → 16k → 44.1k →
// 48k → 8k，rate_near 逐档重试）+ 线性插值重采样，语义等价（保音高保
// 时长）且不引入第三方依赖。板端实测 ES8323（MCLK 5644800 Hz）拒绝
// 22050/11025，见 artifacts/audio-integration/alsa-audio.md 证据。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#include <alsa/asoundlib.h>

#include "voxorchestra/backend/alsa/alsa_audio_sink.hpp"

namespace voxorchestra::backend::alsa {

struct AlsaAudioSink::Impl {
  std::string device;
  int sample_rate = 0;       // PCM 输入采样率（构造注入）
  int actual_rate = 0;       // rate_near 后的硬件实际采样率
  snd_pcm_t* pcm = nullptr;  // 生命周期由 open/close 管理
  snd_pcm_uframes_t period_frames = 0;
};

AlsaAudioSink::AlsaAudioSink(std::string device, int sample_rate)
    : impl_(std::make_unique<Impl>()) {
  impl_->device = std::move(device);
  impl_->sample_rate = sample_rate;
}

AlsaAudioSink::~AlsaAudioSink() { close(); }

namespace {

// 线性插值重采样（16-bit 单声道）：in_rate → out_rate，保音高保时长。
// 输出长度 = round(in * out_rate / in_rate)；边界沿用线性外推（上游
// LinearResample 是带限 sinc，语音场景线性插值质量足够且无第三方依赖）。
std::vector<int16_t> ResampleLinear(const std::vector<int16_t>& in,
                                    int in_rate, int out_rate) {
  if (in.empty() || in_rate == out_rate || in_rate <= 0 || out_rate <= 0) {
    return in;
  }
  const double step = static_cast<double>(in_rate) / out_rate;  // 输入步进
  const std::size_t out_n = static_cast<std::size_t>(std::llround(
      static_cast<double>(in.size()) * out_rate / in_rate));
  std::vector<int16_t> out(out_n);
  double pos = 0.0;
  for (std::size_t i = 0; i < out_n; ++i) {
    const std::size_t k = static_cast<std::size_t>(pos);
    const double frac = pos - k;
    double v = in[k];
    if (k + 1 < in.size()) {
      v += frac * (static_cast<double>(in[k + 1]) - v);
    }
    out[i] = static_cast<int16_t>(v);
    pos += step;
  }
  return out;
}

}  // namespace

bool AlsaAudioSink::open() {
  if (impl_->pcm != nullptr) {
    return false;  // 已打开，未 close 前禁止重复 open（与 FakeAudioSink 一致）
  }
  if (impl_->sample_rate <= 0) {
    return false;
  }

  // 候选率阶梯：requested 优先；板端 ES8323 不支持 22050/11025 时逐档回退
  // （rate_near 在 PCM 层接受 22050 但 codec DAI 在 hw_params 才拒绝，故
  // 必须以 hw_params 成功为准）。每档重开句柄，避免失败状态残留。
  const int candidates[] = {impl_->sample_rate, 16000, 44100, 48000, 8000};
  for (const int candidate : candidates) {
    impl_->actual_rate = 0;
    impl_->period_frames = 0;
    if (snd_pcm_open(&impl_->pcm, impl_->device.c_str(),
                     SND_PCM_STREAM_PLAYBACK, 0) < 0) {
      impl_->pcm = nullptr;
      return false;  // 设备打不开是硬错误，不因采样率改变而恢复
    }
    snd_pcm_hw_params_t* params;
    snd_pcm_hw_params_alloca(&params);
    // 注意：plug 设备（default）的 hw_params_any 返回 1（非负非 0）仍是
    // 有效配置空间，仅负数才是错误（errno 惯例）；首版用 == 0 判定导致
    // default 设备整档误判失败，见板端探针调试记录。
    bool ok = snd_pcm_hw_params_any(impl_->pcm, params) >= 0 &&
              snd_pcm_hw_params_set_access(
                  impl_->pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED) >= 0 &&
              snd_pcm_hw_params_set_format(impl_->pcm, params,
                                           SND_PCM_FORMAT_S16_LE) >= 0 &&
              snd_pcm_hw_params_set_channels(impl_->pcm, params, 1) >= 0;
    if (ok) {
      uint32_t rate = static_cast<uint32_t>(candidate);
      ok = snd_pcm_hw_params_set_rate_near(impl_->pcm, params, &rate, 0) >= 0;
      if (ok) {
        ok = snd_pcm_hw_params(impl_->pcm, params) >= 0;
      }
    }
    if (ok) {
      uint32_t actual = 0;
      snd_pcm_hw_params_get_rate(params, &actual, 0);
      impl_->actual_rate = static_cast<int>(actual);
      snd_pcm_hw_params_get_period_size(params, &impl_->period_frames, 0);
      if (impl_->actual_rate != impl_->sample_rate) {
        std::fprintf(stderr,
                     "[alsa] %s: 采样率 %d 硬件不支持，回退 %d（线性重采样）\n",
                     impl_->device.c_str(), impl_->sample_rate,
                     impl_->actual_rate);
      }
      return true;
    }
    snd_pcm_close(impl_->pcm);
    impl_->pcm = nullptr;
  }
  std::fprintf(stderr, "[alsa] %s: 所有候选采样率均配置失败\n",
               impl_->device.c_str());
  return false;
}

bool AlsaAudioSink::write_pcm(const std::vector<int16_t>& pcm) {
  if (impl_->pcm == nullptr) {
    return false;  // 未 open（与 FakeAudioSink 一致）
  }
  if (impl_->period_frames == 0) {
    return false;
  }

  // 采样率回退时线性重采样（保音高保时长）；与硬件一致时原样直通。
  const std::vector<int16_t>* data = &pcm;
  std::vector<int16_t> resampled;
  if (impl_->actual_rate != impl_->sample_rate) {
    resampled = ResampleLinear(pcm, impl_->sample_rate, impl_->actual_rate);
    data = &resampled;
  }

  // 按 period 分块写；-EPIPE（underrun）→ prepare 恢复后重试该块一次。
  std::size_t offset = 0;
  while (offset < data->size()) {
    const snd_pcm_uframes_t n = static_cast<snd_pcm_uframes_t>(
        std::min<std::size_t>(impl_->period_frames, data->size() - offset));
    snd_pcm_sframes_t written =
        snd_pcm_writei(impl_->pcm, data->data() + offset, n);
    if (written == -EPIPE) {
      snd_pcm_prepare(impl_->pcm);
      written = snd_pcm_writei(impl_->pcm, data->data() + offset, n);
    }
    if (written < 0) {
      std::fprintf(stderr, "[alsa] writei 失败: %s\n", snd_strerror(written));
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

bool AlsaAudioSink::close() {
  if (impl_->pcm == nullptr) {
    return true;  // 幂等：未 open 或已关闭均为空操作（与 FakeAudioSink 一致）
  }
  // drain：等缓冲中全部采样播完再关（上游 AlsaPlay::Drain 语义照抄）。
  bool ok = true;
  const int drain_err = snd_pcm_drain(impl_->pcm);
  if (drain_err < 0) {
    std::fprintf(stderr, "[alsa] drain 失败: %s\n", snd_strerror(drain_err));
    ok = false;
  }
  if (snd_pcm_close(impl_->pcm) < 0) {
    ok = false;
  }
  impl_->pcm = nullptr;
  return ok;
}

int AlsaAudioSink::actual_sample_rate() const { return impl_->actual_rate; }

}  // namespace voxorchestra::backend::alsa
