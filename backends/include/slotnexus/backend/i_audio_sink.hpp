// 音频输出后端契约（PCM 消费端）。
//
// 消费 ITtsBackend 产出的 PCM 帧，输出到具体设备（WAV 文件 / 板端 ALSA）。
// 生命周期固定为 open → write_pcm × N → close；目标（文件路径/设备名）由
// 具体实现构造时注入，不进入接口。
//
// 可替换实现：FakeAudioSink（WAV 文件，默认） / AlsaAudioSink（板端，）。
#pragma once

#include <cstdint>
#include <vector>

namespace slotnexus::backend {

class IAudioSink {
public:
 virtual ~IAudioSink() = default;

 // 打开输出目标；未 close 时重复 open 返回 false（实现相关资源已占用）。
 virtual bool open() = 0;

 // 写入一帧 16-bit 单声道 PCM；未 open 返回 false。
 virtual bool write_pcm(const std::vector<int16_t>& pcm) = 0;

 // 关闭并完成输出（如 WAV 头回填）；未 open 为空操作返回 true；可重复调用。
 virtual bool close() = 0;
};

}  // namespace slotnexus::backend
