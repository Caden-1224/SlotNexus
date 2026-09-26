// AlsaAudioSource：板端 ALSA 录音（独立类，不入 IAudioSink 契约）。
//
// 会话流水线当前输入是 WAV 文件（FakeAsrBackend / asr_node 负载约定），
// 不定义 i_audio_source.hpp——录音侧契约留待  真机联调时
// 按需定义（麦克风 → 会话输入链路）。本类先提供确定性采集原语，供
// alsa_audio_test 与录音验证使用。
//
// 参考上游 sherpa-onnx Alsa（板端 ~/workspace/upstream_rkllm/sherpa-onnx/
// sherpa-onnx/csrc/alsa.cc，开发机同源副本见主机参考目录）：
//  - 阻塞模式 snd_pcm_open(SND_PCM_STREAM_CAPTURE)；
//  - ACCESS_RW_INTERLEAVED / S16_LE / 单声道 / rate_near（默认 16000）；
//  - read() 返回 int16 帧（与 IAsrBackend::feed_audio 输入类型一致；
//    上游 Alsa 返回 float 是它内部对 ASR 的归一化职责，不照抄）；
//  - -EPIPE（overrun/XRUN）→ snd_pcm_prepare 恢复，本次返回空帧（上游
//    Alsa::Read 同款处理：恢复后返回空数据让调用方重试）。
//
// 设备名由构造注入（产品代码不写死本机设备）；板端 ES8323 录音
// 默认 "default"，显式 plughw:0,0 亦可。
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace slotnexus::backend::alsa {

class AlsaAudioSource {
public:
 // device：ALSA PCM 设备名；sample_rate：期望采样率（默认 16000，
 // 与全项目音频常量一致）；实际率由 rate_near 兜底决定。
 AlsaAudioSource(std::string device, int sample_rate = 16000);

 // 析构自动 close（保证异常路径不泄漏 pcm 句柄）。
 ~AlsaAudioSource();

 AlsaAudioSource(const AlsaAudioSource&) = delete;
 AlsaAudioSource& operator=(const AlsaAudioSource&) = delete;

 // 打开采集设备；未 close 时重复 open 返回 false（与 IAudioSink 同款语义）。
 bool open();

 // 读取至多 samples 个采样（int16 单声道）；返回实际读到的采样数，
 // 长度可能小于请求（设备缓冲不足时）；overrun 恢复后返回空帧；
 // 未 open 或读取失败返回空帧。
 std::vector<int16_t> read(int samples);

 // 关闭采集设备；未 open 返回 true；可重复调用（幂等）。
 bool close();

 // 硬件实际采样率（open 成功后有效；未 open 或 open 失败返回 0）。
 int actual_sample_rate() const;

private:
 // snd_pcm_t* 等 ALSA 类型只存在于 .cpp（pimpl 隔离上游类型）。
 struct Impl;
 std::unique_ptr<Impl> impl_;
};

}  // namespace slotnexus::backend::alsa
