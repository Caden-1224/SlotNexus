// AlsaAudioSource 实现：板端 ALSA 录音。
//
// 参数设置与错误处理对齐上游 sherpa-onnx Alsa（csrc/alsa.cc）：
//   - 阻塞模式 snd_pcm_open（不置 SND_PCM_NONBLOCK）；
//   - ACCESS_RW_INTERLEAVED / S16_LE / 单声道 / rate_near；
//   - -EPIPE（overrun）→ 状态检查后 snd_pcm_prepare 恢复，返回空帧让
//     调用方重试（上游 Alsa::Read 对 overrun 的静态计数/退出不照抄——
//     那是 Demo 用法的防御，本类以空帧返回为契约）。
// 与上游的差异：返回值是 int16（见类头注释），不做 float 归一化与重采样。
#include <cstdint>
#include <cstdio>
#include <utility>

#include <alsa/asoundlib.h>

#include "voxorchestra/backend/alsa/alsa_audio_source.hpp"

namespace voxorchestra::backend::alsa {

struct AlsaAudioSource::Impl {
  std::string device;
  int sample_rate = 0;       // 期望采样率（构造注入）
  int actual_rate = 0;       // rate_near 后的硬件实际采样率
  snd_pcm_t* pcm = nullptr;  // 生命周期由 open/close 管理
};

AlsaAudioSource::AlsaAudioSource(std::string device, int sample_rate)
    : impl_(std::make_unique<Impl>()) {
  impl_->device = std::move(device);
  impl_->sample_rate = sample_rate;
}

AlsaAudioSource::~AlsaAudioSource() { close(); }

bool AlsaAudioSource::open() {
  if (impl_->pcm != nullptr) {
    return false;  // 已打开，未 close 前禁止重复 open
  }
  if (impl_->sample_rate <= 0) {
    return false;
  }

  const auto fail = [this]() {
    if (impl_->pcm != nullptr) {
      snd_pcm_close(impl_->pcm);
      impl_->pcm = nullptr;
    }
    impl_->actual_rate = 0;
    return false;
  };

  if (snd_pcm_open(&impl_->pcm, impl_->device.c_str(), SND_PCM_STREAM_CAPTURE,
                   0) < 0) {
    return false;
  }

  snd_pcm_hw_params_t* params;
  snd_pcm_hw_params_alloca(&params);
  if (snd_pcm_hw_params_any(impl_->pcm, params) < 0) {
    return fail();
  }
  if (snd_pcm_hw_params_set_access(impl_->pcm, params,
                                   SND_PCM_ACCESS_RW_INTERLEAVED) < 0) {
    return fail();
  }
  if (snd_pcm_hw_params_set_format(impl_->pcm, params, SND_PCM_FORMAT_S16_LE) <
      0) {
    return fail();
  }
  if (snd_pcm_hw_params_set_channels(impl_->pcm, params, 1) < 0) {
    return fail();
  }
  uint32_t rate = static_cast<uint32_t>(impl_->sample_rate);
  if (snd_pcm_hw_params_set_rate_near(impl_->pcm, params, &rate, 0) < 0) {
    return fail();
  }
  if (snd_pcm_hw_params(impl_->pcm, params) < 0) {
    return fail();
  }
  uint32_t actual = 0;
  snd_pcm_hw_params_get_rate(params, &actual, 0);
  impl_->actual_rate = static_cast<int>(actual);
  if (snd_pcm_prepare(impl_->pcm) < 0) {
    return fail();
  }
  if (impl_->actual_rate != impl_->sample_rate) {
    std::fprintf(stderr,
                 "[alsa] %s: 采样率 %d 就近取为 %d（录音，未重采样）\n",
                 impl_->device.c_str(), impl_->sample_rate, impl_->actual_rate);
  }
  return true;
}

std::vector<int16_t> AlsaAudioSource::read(int samples) {
  if (impl_->pcm == nullptr || samples <= 0) {
    return {};
  }
  std::vector<int16_t> out(static_cast<std::size_t>(samples));
  const snd_pcm_sframes_t n =
      snd_pcm_readi(impl_->pcm, out.data(), static_cast<snd_pcm_uframes_t>(samples));
  if (n == -EPIPE) {
    // overrun：XRUN 状态下 prepare 恢复，返回空帧（调用方重试）。
    // 注意：用户态只有 SND_PCM_STATE_XRUN（覆盖 underrun/overrun），
    // SNDRV_PCM_STATE_OVERRUN 是内核头名，libasound 不导出。
    if (snd_pcm_state(impl_->pcm) == SND_PCM_STATE_XRUN) {
      snd_pcm_prepare(impl_->pcm);
    }
    return {};
  }
  if (n < 0) {
    std::fprintf(stderr, "[alsa] readi 失败: %s\n", snd_strerror(n));
    return {};
  }
  out.resize(static_cast<std::size_t>(n));
  return out;
}

bool AlsaAudioSource::close() {
  if (impl_->pcm == nullptr) {
    return true;  // 幂等：未 open 或已关闭均为空操作
  }
  const int err = snd_pcm_close(impl_->pcm);
  impl_->pcm = nullptr;
  return err == 0;
}

int AlsaAudioSource::actual_sample_rate() const { return impl_->actual_rate; }

}  // namespace voxorchestra::backend::alsa
