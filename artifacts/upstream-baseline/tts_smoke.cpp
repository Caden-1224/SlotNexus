// tts_smoke：SummerTTS vits 冒烟。
//
// 目的：验证 single_speaker_fast.bin + SummerTTS vits 引擎（基于 Eigen 3.4.0，
//      无外部 NN 运行时）能正确合成中英文本为 16 kHz mono S16 PCM，
//      采集模型加载耗时 / 合成耗时 / RTF。是 SummerTtsBackend 的行为依据。
// 来源：合成调用逻辑（ttsLoadModel / SynthesizerTrn::infer）取自
//      LLM_Voice_Flow/tts 的 test/main.cpp（作者注释掉的原始版）与 README 用法，
//      剥离 tts_server 的 ZMQ+ALSA 集成层，改为命令行传模型 + 文本，合成结果落 WAV。
// 依赖：仅 Eigen 3.4.0（tts/eigen-3.4.0 自带头文件）+ -fopenmp；不链接 ZMQ/ALSA/PortAudio。
// 输出：16 kHz mono 16-bit WAV（44 字节头 + PCM），采样点数 = infer 的 dataLen。
// 编译：见同目录 CMakeLists.smoke.txt（板端 aarch64 原生 g++，与 tts 核心 src/ 同编）。
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "SynthesizerTrn.h"
#include "tts_file_io.h"
#include "utils.h"

namespace {
void put16(std::ofstream& f, uint16_t v) {
  f.put(static_cast<char>(v & 0xFF));
  f.put(static_cast<char>((v >> 8) & 0xFF));
}
void put32(std::ofstream& f, uint32_t v) {
  f.put(static_cast<char>(v & 0xFF));
  f.put(static_cast<char>((v >> 8) & 0xFF));
  f.put(static_cast<char>((v >> 16) & 0xFF));
  f.put(static_cast<char>((v >> 24) & 0xFF));
}

// 写 44 字节头 RIFF/WAVE，PCM 16-bit mono。
bool writeWavPcm16(const char* path, const int16_t* data, int32_t n, uint32_t sr) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  const uint16_t channels = 1;
  const uint16_t bits = 16;
  const uint16_t blockAlign = channels * (bits / 8);
  const uint32_t byteRate = sr * blockAlign;
  const uint32_t dataSize = static_cast<uint32_t>(n) * 2u;
  const uint32_t riffSize = 36u + dataSize;

  f.write("RIFF", 4);
  put32(f, riffSize);
  f.write("WAVE", 4);
  f.write("fmt ", 4);
  put32(f, 16);             // PCM fmt chunk size
  put16(f, 1);              // audioFormat = PCM
  put16(f, channels);
  put32(f, sr);
  put32(f, byteRate);
  put16(f, blockAlign);
  put16(f, bits);
  f.write("data", 4);
  put32(f, dataSize);
  f.write(reinterpret_cast<const char*>(data), dataSize);
  return static_cast<bool>(f);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr,
                 "Usage: %s <model.bin> \"<text>\" <out.wav> [lengthScale]\n",
                 argv[0]);
    return 1;
  }
  char* model_path = argv[1];
  const std::string text = argv[2];
  const char* out_path = argv[3];
  const float lengthScale = (argc >= 5) ? std::strtof(argv[4], nullptr) : 1.0f;

  // 加载模型 + 计时
  auto t0 = std::chrono::steady_clock::now();
  float* modelData = nullptr;
  int32_t modelSize = ttsLoadModel(model_path, &modelData);
  auto t1 = std::chrono::steady_clock::now();
  if (modelSize <= 0 || !modelData) {
    std::fprintf(stderr, "ttsLoadModel failed: size=%d path=%s\n", modelSize,
                 model_path);
    return 2;
  }
  const double load_s = std::chrono::duration<double>(t1 - t0).count();
  std::fprintf(stderr, "model loaded: size=%d load=%.3fs\n", modelSize, load_s);

  SynthesizerTrn synth(modelData, modelSize);

  // 合成 + 计时
  int32_t dataLen = 0;
  auto t2 = std::chrono::steady_clock::now();
  int16_t* wav = synth.infer(text, 0, lengthScale, dataLen);
  auto t3 = std::chrono::steady_clock::now();
  const double infer_s = std::chrono::duration<double>(t3 - t2).count();

  if (!wav || dataLen <= 0) {
    std::fprintf(stderr, "infer failed: dataLen=%d text=\"%s\"\n", dataLen,
                 text.c_str());
    tts_free_data(modelData);
    return 3;
  }

  const uint32_t sr = 16000;
  const double audio_s = static_cast<double>(dataLen) / sr;
  const double rtf = (audio_s > 0) ? infer_s / audio_s : 0.0;

  std::fprintf(stderr,
               "infer: dataLen=%d audio=%.3fs infer=%.3fs RTF=%.3f lengthScale=%.2f\n",
               dataLen, audio_s, infer_s, rtf, lengthScale);

  if (!writeWavPcm16(out_path, wav, dataLen, sr)) {
    std::fprintf(stderr, "write wav failed: %s\n", out_path);
    tts_free_data(wav);
    tts_free_data(modelData);
    return 4;
  }
  std::fprintf(stderr, "wav written: %s (%d samples, %zu bytes)\n", out_path,
               dataLen, static_cast<size_t>(44 + dataLen * 2));

  tts_free_data(wav);
  tts_free_data(modelData);
  return 0;
}
