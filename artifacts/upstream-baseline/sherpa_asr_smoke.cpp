// sherpa_asr_smoke：sherpa-onnx streaming zipformer 冒烟。
//
// 目的：验证 libsherpa-onnx-c-api.so + 中英双语 streaming-zipformer int8 模型
//      能正确识别固定 WAV，采集 RTF。是 SherpaAsrBackend 的 c-api 行为依据。
// 来源：调用模式取自 c-api-examples/streaming-zipformer-c-api.c，改为命令行传入
//      模型目录与 WAV，并加 RTF 计时。
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "sherpa-onnx/c-api/c-api.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "Usage: %s <model_dir> <wav> [num_threads]\n", argv[0]);
    return 1;
  }
  const std::string dir = argv[1];
  const std::string wav_path = argv[2];
  const int num_threads = (argc >= 4) ? std::atoi(argv[3]) : 2;

  const std::string enc = dir + "/encoder-epoch-99-avg-1.int8.onnx";
  const std::string dec = dir + "/decoder-epoch-99-avg-1.int8.onnx";
  const std::string jnr = dir + "/joiner-epoch-99-avg-1.int8.onnx";
  const std::string tok = dir + "/tokens.txt";

  const SherpaOnnxWave* wave = SherpaOnnxReadWave(wav_path.c_str());
  if (!wave) {
    std::fprintf(stderr, "read wave failed: %s\n", wav_path.c_str());
    return 2;
  }
  std::fprintf(stderr, "wav=%s sr=%d n=%d dur=%.2fs\n", wav_path.c_str(),
               wave->sample_rate, wave->num_samples,
               (float)wave->num_samples / wave->sample_rate);

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
  rc.feat_config.sample_rate = 16000;
  rc.feat_config.feature_dim = 80;
  rc.model_config = mc;
  rc.decoding_method = "greedy_search";
  rc.max_active_paths = 4;
  rc.enable_endpoint = 0;  // 整段识别，不按端点切段（由 IAsrBackend 的 is_last 控制）

  auto t0 = std::chrono::steady_clock::now();
  const SherpaOnnxOnlineRecognizer* rec = SherpaOnnxCreateOnlineRecognizer(&rc);
  if (!rec) {
    std::fprintf(stderr, "create recognizer failed\n");
    SherpaOnnxFreeWave(wave);
    return 3;
  }
  const SherpaOnnxOnlineStream* stream = SherpaOnnxCreateOnlineStream(rec);

  // 模拟流式：每 N=3200 样本（0.2s）一块。
  const int N = 3200;
  int k = 0;
  while (k < wave->num_samples) {
    const int end = (k + N > wave->num_samples) ? wave->num_samples : (k + N);
    SherpaOnnxOnlineStreamAcceptWaveform(stream, wave->sample_rate,
                                         wave->samples + k, end - k);
    while (SherpaOnnxIsOnlineStreamReady(rec, stream)) {
      SherpaOnnxDecodeOnlineStream(rec, stream);
    }
    const SherpaOnnxOnlineRecognizerResult* r =
        SherpaOnnxGetOnlineStreamResult(rec, stream);
    std::fprintf(stderr, "\rpartial: %-60s", r->text);
    SherpaOnnxDestroyOnlineRecognizerResult(r);
    k = end;
  }

  // 尾部 0.3s 静音 padding，再 InputFinished，做最终 decode。
  std::vector<float> pad(4800, 0.0f);
  SherpaOnnxOnlineStreamAcceptWaveform(stream, wave->sample_rate, pad.data(), 4800);
  SherpaOnnxOnlineStreamInputFinished(stream);
  while (SherpaOnnxIsOnlineStreamReady(rec, stream)) {
    SherpaOnnxDecodeOnlineStream(rec, stream);
  }
  const auto t1 = std::chrono::steady_clock::now();

  const SherpaOnnxOnlineRecognizerResult* r =
      SherpaOnnxGetOnlineStreamResult(rec, stream);
  const double infer_s = std::chrono::duration<double>(t1 - t0).count();
  const double audio_s = (double)wave->num_samples / wave->sample_rate;
  std::fprintf(stderr, "\n=== final ===\n");
  std::fprintf(stderr, "text: %s\n", r->text);
  std::fprintf(stderr, "audio_s: %.2f  infer_s: %.2f  RTF: %.3f  threads: %d\n",
               audio_s, infer_s, infer_s / audio_s, num_threads);
  SherpaOnnxDestroyOnlineRecognizerResult(r);

  SherpaOnnxDestroyOnlineStream(stream);
  SherpaOnnxDestroyOnlineRecognizer(rec);
  SherpaOnnxFreeWave(wave);
  return 0;
}
