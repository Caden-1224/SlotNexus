// Fake 语音后端集合：确定性 ASR/LLM/TTS 与 WAV 输出。
// Author: Caden
//
// 只服务嵌入式默认模式和 WSL/Mock 回归，不模拟真实模型或声卡。
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "slotnexus/backend/i_asr_backend.hpp"
#include "slotnexus/backend/i_audio_sink.hpp"
#include "slotnexus/backend/i_llm_backend.hpp"
#include "slotnexus/backend/i_tts_backend.hpp"

namespace slotnexus::backend::fake {

class FakeAsrBackend final : public IAsrBackend {
 public:
  void set_event_callback(EventCallback cb) override {
    cb_ = std::move(cb);
    frame_count_ = 0;
    partials_.clear();
    cancelled_.store(false);
  }

  void feed_audio(const std::vector<int16_t>& pcm, bool is_last) override {
    if (!cb_ || cancelled_.load()) {
      return;
    }
    ++frame_count_;
    const std::string partial =
        "第" + std::to_string(frame_count_) + "帧(" + std::to_string(pcm.size()) + ")";
    partials_.push_back(partial);
    cb_({BackendEvent::Kind::kPartial, partial, {}});

    if (is_last) {
      std::string final_text;
      for (std::size_t i = 0; i < partials_.size(); ++i) {
        if (i > 0) {
          final_text += " ";
        }
        final_text += partials_[i];
      }
      cb_({BackendEvent::Kind::kFinal, std::move(final_text), {}});
    }
  }

  void cancel() override { cancelled_.store(true); }

 private:
  EventCallback cb_;
  std::atomic<bool> cancelled_{false};
  int frame_count_ = 0;
  std::vector<std::string> partials_;
};

// FakeAudioSource：确定性麦克风模拟（合成 16-bit 单声道 PCM 帧）。
// Author: Caden
//
// 帧内容完全由（帧序号, 采样下标）决定，不含随机源：相同参数永远产出
// 相同数据，供协议、状态机与编排测试断言精确内容。不是真实采集设备。
class FakeAudioSource {
 public:
  // 生成第 seq 帧（默认 20 ms 帧 = 320 采样）。
  // 采样值 = ((seq*977 + 下标*197) mod 2048) - 1024，范围 [-1024, 1023]。
  static std::vector<int16_t> make_frame(std::uint32_t seq,
                                         int samples = kFrameSamples) {
    std::vector<int16_t> frame(static_cast<std::size_t>(samples));
    for (int s = 0; s < samples; ++s) {
      frame[static_cast<std::size_t>(s)] = static_cast<int16_t>(
          ((seq * 977 + static_cast<std::uint32_t>(s) * 197) % 2048) - 1024);
    }
    return frame;
  }
};

class FakeLlmBackend final : public ILlmBackend {
 public:
  void set_event_callback(EventCallback cb) override {
    cb_ = std::move(cb);
    cancelled_.store(false);
  }

  void generate(const std::string& prompt) override {
    if (!cb_ || cancelled_.load()) {
      return;
    }
    const auto tokens = split_words(prompt);
    std::string final_text;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
      cb_({BackendEvent::Kind::kToken, tokens[i], {}});
      if (i > 0) {
        final_text += " ";
      }
      final_text += tokens[i];
    }
    cb_({BackendEvent::Kind::kDone, std::move(final_text), {}});
  }

  void cancel() override { cancelled_.store(true); }

 private:
  // 按空白切分（连续空白折叠），保持 token 顺序；空串返回空列表。
  static std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> words;
    std::istringstream iss(s);
    std::string w;
    while (iss >> w) {
      words.push_back(w);
    }
    return words;
  }

  EventCallback cb_;
  std::atomic<bool> cancelled_{false};
};

class FakeTtsBackend final : public ITtsBackend {
 public:
  void set_event_callback(EventCallback cb) override {
    cb_ = std::move(cb);
    cancelled_.store(false);
  }

  void synthesize(const std::string& text) override {
    if (!cb_ || cancelled_.load()) {
      return;
    }
    const std::size_t chunk_count =
        std::max<std::size_t>(1, (text.size() + 31) / 32);  // ⌈字节数/32⌉
    for (std::size_t c = 0; c < chunk_count; ++c) {
      std::vector<int16_t> pcm(static_cast<std::size_t>(kFrameSamples));
      for (std::size_t s = 0; s < pcm.size(); ++s) {
        const std::size_t g = c * static_cast<std::size_t>(kFrameSamples) + s;
        pcm[s] = (g % 32 < 16) ? 6000 : -6000;  // 500 Hz 方波，相位跨块连续
      }
      cb_({BackendEvent::Kind::kPcm, {}, std::move(pcm)});
    }
    cb_({BackendEvent::Kind::kDone, {}, {}});
  }

  void cancel() override { cancelled_.store(true); }

 private:
  EventCallback cb_;
  std::atomic<bool> cancelled_{false};
};

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

}  // namespace slotnexus::backend::fake
