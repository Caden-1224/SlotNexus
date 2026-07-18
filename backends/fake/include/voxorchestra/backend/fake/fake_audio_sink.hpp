// FakeAudioSink：把 PCM 帧写出为标准 WAV 文件（默认 Mock 音频输出）。
//
// 生命周期（与 IAudioSink 契约一致）：
//   open → write_pcm × N → close；未 close 重复 open 返回 false；
//   未 open 的 write_pcm 返回 false；close 幂等（可重复调用、未 open 也返回 true）。
//
// 输出为 16-bit 单声道 16 kHz RIFF WAV（44 字节标准头）；open 时写入
// 占位头，close 时回填 RIFF/数据段长度。析构时自动 close（保证异常路径
// 也不产生损坏的半成品文件）。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "voxorchestra/backend/backend_event.hpp"
#include "voxorchestra/backend/i_audio_sink.hpp"

namespace voxorchestra::backend::fake {

class FakeAudioSink final : public IAudioSink {
 public:
  // 目标文件路径由构造注入（产品代码不写死本机路径）。
  explicit FakeAudioSink(std::string path) : path_(std::move(path)) {}

  ~FakeAudioSink() override { close(); }

  FakeAudioSink(const FakeAudioSink&) = delete;
  FakeAudioSink& operator=(const FakeAudioSink&) = delete;

  bool open() override {
    if (file_ != nullptr) {
      return false;  // 已打开，未 close 前禁止重复 open
    }
    file_ = std::fopen(path_.c_str(), "wb");
    if (file_ == nullptr) {
      return false;
    }
    data_bytes_ = 0;
    // 写入 44 字节零占位头，close 时回填真实长度。
    const std::uint8_t placeholder[44] = {};
    if (std::fwrite(placeholder, 1, sizeof(placeholder), file_) !=
        sizeof(placeholder)) {
      std::fclose(file_);
      file_ = nullptr;
      return false;
    }
    return true;
  }

  bool write_pcm(const std::vector<int16_t>& pcm) override {
    if (file_ == nullptr) {
      return false;
    }
    const std::size_t bytes = pcm.size() * sizeof(int16_t);
    if (std::fwrite(pcm.data(), 1, bytes, file_) != bytes) {
      return false;
    }
    data_bytes_ += bytes;
    return true;
  }

  bool close() override {
    if (file_ == nullptr) {
      return true;  // 幂等：未 open 或已关闭均为空操作
    }
    const bool ok = WriteHeader(file_, data_bytes_) == 0 && std::fclose(file_) == 0;
    file_ = nullptr;
    return ok;
  }

 private:
  // 回填 44 字节 RIFF WAV 头；失败返回非零。
  static int WriteHeader(std::FILE* f, std::uint32_t data_bytes) {
    if (std::fseek(f, 0, SEEK_SET) != 0) {
      return -1;
    }
    const std::uint32_t riff_size = 36 + data_bytes;
    const std::uint32_t byte_rate =
        static_cast<std::uint32_t>(kSampleRateHz) * kChannels * 2;
    const std::uint16_t block_align = static_cast<std::uint16_t>(kChannels * 2);
    const std::uint16_t bits = 16;

    if (std::fwrite("RIFF", 1, 4, f) != 4 || WriteLe32(f, riff_size) != 0 ||
        std::fwrite("WAVE", 1, 4, f) != 4 || std::fwrite("fmt ", 1, 4, f) != 4 ||
        WriteLe32(f, 16) != 0 || WriteLe16(f, 1) != 0 ||
        WriteLe16(f, static_cast<std::uint16_t>(kChannels)) != 0 ||
        WriteLe32(f, static_cast<std::uint32_t>(kSampleRateHz)) != 0 ||
        WriteLe32(f, byte_rate) != 0 || WriteLe16(f, block_align) != 0 ||
        WriteLe16(f, bits) != 0 || std::fwrite("data", 1, 4, f) != 4 ||
        WriteLe32(f, data_bytes) != 0) {
      return -1;
    }
    return 0;
  }

  static int WriteLe32(std::FILE* f, std::uint32_t v) {
    const std::uint8_t b[4] = {static_cast<std::uint8_t>(v),
                               static_cast<std::uint8_t>(v >> 8),
                               static_cast<std::uint8_t>(v >> 16),
                               static_cast<std::uint8_t>(v >> 24)};
    return std::fwrite(b, 1, 4, f) == 4 ? 0 : -1;
  }

  static int WriteLe16(std::FILE* f, std::uint16_t v) {
    const std::uint8_t b[2] = {static_cast<std::uint8_t>(v),
                               static_cast<std::uint8_t>(v >> 8)};
    return std::fwrite(b, 1, 2, f) == 2 ? 0 : -1;
  }

  std::string path_;
  std::FILE* file_ = nullptr;
  std::uint32_t data_bytes_ = 0;
};

}  // namespace voxorchestra::backend::fake
