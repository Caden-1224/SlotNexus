// AlsaAudioSink：板端 ALSA 播放输出（IAudioSink 实现，）。
// Author: Caden
//
// 与 FakeAudioSink（WAV）逐项语义对照（契约见 i_audio_sink.hpp）：
//  FakeAudioSink（WAV 文件）       | AlsaAudioSink（ALSA PCM 设备）
//  -------------------------------+---------------------------------------
//  open：fopen + 44B 占位头        | snd_pcm_open(PLAYBACK) + S16_LE/立体声/
//                                 |   rate_near（按 hw 默认 period 分块）
//  未 close 重复 open → false      | 同左（同一 pcm 句柄已占用）
//  write_pcm：fwrite 整块          | 单声道复制到左右后按 period 分块
//                                 |   snd_pcm_writei；
//                                 |   -EPIPE（underrun）→ prepare 后重试
//  未 open write_pcm → false       | 同左
//  close：回填 RIFF 头 + fclose    | snd_pcm_drain（等全部播完）+ close
//  未 open close → true（空操作）  | 同左
//  close 幂等（重复调用 → true）    | 同左
//  析构自动 close（异常路径安全）   | 同左
//
// 采样率：构造注入 PCM 输入率（tts_node 传合成端输出率 = 16000，与契约
// kSampleRateHz 一致；MeloTTS 经板端重采样到该率，Fake 同率。
// F0 实测确认为 16 kHz——非 LJ Speech/VITS 常见的 22050，英文模型才是）。
// open 时 snd_pcm_hw_params_set_rate_near 兜底取硬件就近值；板端 ES8323
// codec 在 5644800 Hz MCLK 下不支持 22050/11025（内核 -EINVAL，实测
// 8000/16000/24000/32000/44100/48000 可用），输入率被硬件拒绝时 open
// 依次回退候选率并做线性重采样保证音高/时长正确（对齐上游 AlsaPlay 的
// resampler 职责），actual_sample_rate() 返回硬件实际值。
//
// 声道：设备按立体声打开，write_pcm 的单声道输入复制到左右声道。板端
// ES8388 单声道播放的实际时基是协商采样率的 2 倍（3 s 音频 drain 只用
// 1.5 s，语音变尖变快），立体声协商值与实际一致；采集侧单声道正常。
// 对外契约不变：入参与计数都按单声道 16-bit 采样。
//
// 设备名由构造注入（产品代码不写死本机设备）：tts_node --sink-device
// 默认 "default"，板端显式 plughw:0,0（ES8323）。16000 在两设备均原生
// 支持、直通不重采样；回退+重采样路径仅为 codec 不支持的输入率兜底。
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "slotnexus/backend/i_audio_sink.hpp"

namespace slotnexus::backend::alsa {

class AlsaAudioSink final : public IAudioSink {
public:
 // device：ALSA PCM 设备名（"default" / "plughw:0,0"）。
 // sample_rate：PCM 输入采样率（MeloTTS 重采样后 / Fake，契约 kSampleRateHz）。
 // 注意：本类只做播放，不做重采样；若采样率与硬件能力不一致，
 // rate_near 就近取值并记录，调用方按 actual_sample_rate() 解释数据。
 AlsaAudioSink(std::string device, int sample_rate);

 // 析构自动 close（保证异常路径不泄漏 pcm 句柄）。
 ~AlsaAudioSink() override;

 AlsaAudioSink(const AlsaAudioSink&) = delete;
 AlsaAudioSink& operator=(const AlsaAudioSink&) = delete;

 bool open() override;
 bool write_pcm(const std::vector<int16_t>& pcm) override;
 bool close() override;

 // 硬件实际采样率（open 成功后有效；未 open 或 open 失败返回 0）。
 int actual_sample_rate() const;

private:
 // snd_pcm_t* 等 ALSA 类型只存在于 .cpp（pimpl 隔离上游类型）。
 struct Impl;
 std::unique_ptr<Impl> impl_;
};

}  // namespace slotnexus::backend::alsa
