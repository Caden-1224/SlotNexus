// tts_node 可执行入口：语音合成节点（WAV 输出 / ALSA 声卡播放）。
//
// 用法：tts_node [--listen tcp://127.0.0.1:19204] [--output-dir <目录>]
//                [--config <session.json>] [--backend fake|melotts|summertts]
//                [--model <模型路径>] [--length-scale <倍率>]
//                [--encoder-model <onnx>] [--decoder-model <rknn>]
//                [--lexicon <txt>] [--tokens <txt>] [--g-vector <bin>]
//                [--speed <倍率>]
//                [--sink wav|alsa] [--sink-device <设备名>]
//                [--infer-timeout-ms <ms>]
// 默认端口约定：echo 19200 / asr 19201 / rag 19202 / llm 19203 / tts 19204。
//
// Node 外壳（RuntimeNode + TaskRuntime）只依赖接口；本文件实现 IBackend
// 适配器，把流式 ITtsBackend 的 PCM 帧写入输出目标（sink）：
//   - Mock 负载约定：客户端发 {"text": "<文本>"}；RuntimeNode 已提取 text
//     字段，适配器收到纯文本；
//   - --sink wav（默认）：写入 FakeAudioSink（WAV 文件），文件名 =
//     <output-dir>/tts_<FNV-1a 文本哈希>.wav（确定性，相同文本覆盖同名
//     文件），返回 {"wav_path": ..., "pcm_bytes": N}；
//   - --sink alsa：改走 AlsaAudioSink（板端声卡实时播放，不落盘），
//     返回 {"device": ..., "pcm_bytes": N, "sample_rate": 实际值}。
//     sink 采样率 = 合成端输出率 = 契约 kSampleRateHz（16 kHz mono S16）：
//     MeloTTS 原生 44.1 kHz 由后端内部线性重采样到 16 kHz，Fake 同为 16 kHz。
//     --sink-device 默认 "default"，板端显式 plughw:0,0（ES8323）；
//     x86 默认构建在 --sink alsa 时拒绝启动（SLOTNEXUS_HAS_ALSA 门控）。
//
// 后端经工厂注入：--backend fake（默认，x86/Mock 回归基线）、melotts
// （板端真实合成：ONNX Runtime CPU 编码器 + RKNN NPU 解码器，当前默认）或
// summertts（板端真实 vits，需显式提供 SummerTTS 源码根目录）。
// 模型路径经 session.json::tts 或命令行参数注入，不硬编码；每次 setup 产出
// 独立后端实例（TaskRuntime 工厂语义），真实后端实例持有独立模型上下文。
// SIGINT/SIGTERM 优雅退出（退出码 0）。
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <zmq.hpp>

#include "slotnexus/backend/backend_event.hpp"
#include "slotnexus/backend/fake/fake_audio_sink.hpp"
#include "slotnexus/backend/fake/fake_tts_backend.hpp"
#ifdef SLOTNEXUS_HAS_MELOTTS
#include "slotnexus/backend/melotts/melotts_tts_backend.hpp"
#endif
#ifdef SLOTNEXUS_HAS_SUMMERTTS
#include "slotnexus/backend/summer_tts/summer_tts_backend.hpp"
#endif
#ifdef SLOTNEXUS_HAS_ALSA
#include "slotnexus/backend/alsa/alsa_audio_sink.hpp"
#endif
#include "slotnexus/runtime/ibackend.hpp"
#include "runtime_node.hpp"

namespace {

// FNV-1a 32 位哈希 → 8 位十六进制（确定性文件名）。
std::string TextHash(const std::string& s) {
  std::uint32_t h = 2166136261u;
  for (unsigned char c : s) {
    h ^= c;
    h *= 16777619u;
  }
  char buf[9];
  std::snprintf(buf, sizeof(buf), "%08x", h);
  return buf;
}

// IBackend 适配器：合成 → 收集 PCM 块 → 写入输出目标（WAV 文件 / ALSA 声卡）。
class TtsNodeBackend final : public slotnexus::runtime::IBackend {
 public:
  // tts：后端实例（工厂注入，Fake / SummerTTS 可替换）。
  // sink_name / sink_device：输出目标（wav / alsa，设备名）；alsa 模式
  // 不写文件，sink_sample_rate = 合成端输出率（见 main() 注释）。
  TtsNodeBackend(std::unique_ptr<slotnexus::backend::ITtsBackend> tts,
                 std::string output_dir, std::string sink_name,
                 std::string sink_device, int sink_sample_rate)
      : tts_(std::move(tts)),
        output_dir_(std::move(output_dir)),
        sink_name_(std::move(sink_name)),
        sink_device_(std::move(sink_device)),
        sink_sample_rate_(sink_sample_rate) {}

  slotnexus::runtime::BackendResult infer(
      const std::string& payload,
      std::chrono::steady_clock::time_point /*deadline*/,
      const std::atomic<bool>& cancelled,
      const slotnexus::runtime::EventSink& events) override {
    if (cancelled.load()) {
      tts_->cancel();
      return {slotnexus::runtime::BackendResult::Code::kCancelled, {}};
    }
    std::vector<std::vector<int16_t>> chunks;
    tts_->set_event_callback(
        [&chunks, &events](const slotnexus::backend::BackendEvent& e) {
          if (e.kind == slotnexus::backend::BackendEvent::Kind::kPcm) {
            chunks.push_back(e.pcm);
          }
          if (events) {
            events(e);  // PCM 帧实时转发数据面
          }
        });
    tts_->synthesize(payload);

    if (sink_name_ == "alsa") {
      return play_alsa(chunks);
    }
    const std::string wav_path = output_dir_ + "/tts_" + TextHash(payload) + ".wav";
    slotnexus::backend::fake::FakeAudioSink sink(wav_path);
    bool ok = sink.open();
    std::size_t pcm_bytes = 0;
    if (ok) {
      for (const auto& chunk : chunks) {
        ok = sink.write_pcm(chunk) && ok;
        pcm_bytes += chunk.size() * sizeof(int16_t);
      }
      ok = sink.close() && ok;
    }
    nlohmann::json result = {{"wav_path", ok ? wav_path : ""},
                             {"pcm_bytes", pcm_bytes}};
    if (!ok) {
      result["error"] = "WAV 写出失败";
    }
    return {slotnexus::runtime::BackendResult::Code::kOk, result.dump()};
  }

 private:
  // ALSA 播放：AlsaAudioSink 实时写出，不落盘；返回实际采样率
  // （rate_near 兜底值，见 AlsaAudioSink 类头注释）。
  slotnexus::runtime::BackendResult play_alsa(
      const std::vector<std::vector<int16_t>>& chunks) {
#ifdef SLOTNEXUS_HAS_ALSA
    slotnexus::backend::alsa::AlsaAudioSink sink(sink_device_,
                                                    sink_sample_rate_);
    bool ok = sink.open();
    std::size_t pcm_bytes = 0;
    if (ok) {
      for (const auto& chunk : chunks) {
        ok = sink.write_pcm(chunk) && ok;
        pcm_bytes += chunk.size() * sizeof(int16_t);
      }
      ok = sink.close() && ok;
    }
    nlohmann::json result = {{"device", sink_device_},
                             {"pcm_bytes", pcm_bytes},
                             {"sample_rate", sink.actual_sample_rate()}};
    if (!ok) {
      result["error"] = "ALSA 播放失败";
    }
    return {slotnexus::runtime::BackendResult::Code::kOk, result.dump()};
#else
    (void)chunks;
    return {slotnexus::runtime::BackendResult::Code::kOk,
            R"({"error":"当前构建未启用 ALSA sink"})"};
#endif
  }

  std::unique_ptr<slotnexus::backend::ITtsBackend> tts_;
  std::string output_dir_;
  std::string sink_name_;
  std::string sink_device_;
  int sink_sample_rate_;
};

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int /*sig*/) { g_stop = 1; }

// MeloTTS 后端的运行时配置取值（与 session.json::tts 对应）。
// 单独抽成一份纯数据，避免在未启用硬件构建时把厂商头引入节点外壳。
struct MeloConfigValues {
  std::string encoder_model;
  std::string decoder_model;
  std::string lexicon;
  std::string tokens;
  std::string g_vector;
  float speed = 0.8f;
  float noise_scale = 0.3f;
  float noise_scale_w = 0.6f;
  float sdp_ratio = 0.2f;
  int intra_op_threads = 1;
  int native_sample_rate = 44100;
  int max_encoder_phones = 240;
  int run_timeout_ms = 30000;

  // 五个资源路径是否齐全（backend=melotts 启动前置条件）。
  bool complete() const {
    return !encoder_model.empty() && !decoder_model.empty() && !lexicon.empty() &&
           !tokens.empty() && !g_vector.empty();
  }
};

float parse_float(const char* s, float fallback) {
  try {
    return std::stof(s);
  } catch (...) {
    return fallback;
  }
}

int parse_int(const char* s, int fallback) {
  try {
    return std::stoi(s);
  } catch (...) {
    return fallback;
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string listen = "tcp://127.0.0.1:19204";
  std::string output_dir = "tts-out";
  std::string backend_name = "fake";  // 默认 Fake（x86/Mock 回归基线）
  std::string model_path;             // summertts 后端必填
  float length_scale = 1.0f;          // 语速倍率（门禁基线 1.0）
  MeloConfigValues melo;              // melotts 后端配置
  std::string sink_name = "wav";      // 输出目标：wav（默认）/ alsa
  std::string sink_device = "default";  // alsa 设备名（板端 plughw:0,0）
  int infer_timeout_ms = 0;           // 节点内推理超时；0 = 默认 5000 ms
  std::string events_endpoint;        // 数据面事件 PUB 端点（可选）
  std::string events_sync;            // 配套握手端点

  // 先读配置文件（--config 的 tts 段），命令行参数随后覆盖。
  for (int i = 1; i < argc - 1; ++i) {
    if (std::string(argv[i]) == "--config") {
      nlohmann::json file_cfg;
      try {
        std::ifstream in(argv[i + 1]);
        file_cfg = nlohmann::json::parse(in);
      } catch (const std::exception& e) {
        std::cerr << "配置文件读取失败（--config " << argv[i + 1] << "）: "
                  << e.what() << std::endl;
        return 1;
      }
      if (file_cfg.contains("tts")) {
        const auto& t = file_cfg["tts"];
        backend_name = t.value("backend", backend_name);
        model_path = t.value("model", model_path);
        length_scale = t.value("length_scale", length_scale);
        sink_name = t.value("sink", sink_name);            // 可选，缺省 wav
        sink_device = t.value("sink_device", sink_device);
        // MeloTTS 段（backend=melotts 时使用）。
        melo.encoder_model = t.value("encoder_model", melo.encoder_model);
        melo.decoder_model = t.value("decoder_model", melo.decoder_model);
        melo.lexicon = t.value("lexicon", melo.lexicon);
        melo.tokens = t.value("tokens", melo.tokens);
        melo.g_vector = t.value("g_vector", melo.g_vector);
        melo.speed = t.value("speed", melo.speed);
        melo.noise_scale = t.value("noise_scale", melo.noise_scale);
        melo.noise_scale_w = t.value("noise_scale_w", melo.noise_scale_w);
        melo.sdp_ratio = t.value("sdp_ratio", melo.sdp_ratio);
        melo.intra_op_threads = t.value("intra_op_threads", melo.intra_op_threads);
        melo.native_sample_rate =
            t.value("native_sample_rate", melo.native_sample_rate);
        melo.max_encoder_phones =
            t.value("max_encoder_phones", melo.max_encoder_phones);
        melo.run_timeout_ms = t.value("run_timeout_ms", melo.run_timeout_ms);
      }
    }
  }
  for (int i = 1; i < argc - 1; ++i) {
    if (std::string(argv[i]) == "--listen") {
      listen = argv[i + 1];
    } else if (std::string(argv[i]) == "--output-dir") {
      output_dir = argv[i + 1];
    } else if (std::string(argv[i]) == "--backend") {
      backend_name = argv[i + 1];
    } else if (std::string(argv[i]) == "--model") {
      model_path = argv[i + 1];
    } else if (std::string(argv[i]) == "--length-scale") {
      length_scale = parse_float(argv[i + 1], 1.0f);
    } else if (std::string(argv[i]) == "--encoder-model") {
      melo.encoder_model = argv[i + 1];
    } else if (std::string(argv[i]) == "--decoder-model") {
      melo.decoder_model = argv[i + 1];
    } else if (std::string(argv[i]) == "--lexicon") {
      melo.lexicon = argv[i + 1];
    } else if (std::string(argv[i]) == "--tokens") {
      melo.tokens = argv[i + 1];
    } else if (std::string(argv[i]) == "--g-vector") {
      melo.g_vector = argv[i + 1];
    } else if (std::string(argv[i]) == "--speed") {
      melo.speed = parse_float(argv[i + 1], melo.speed);
    } else if (std::string(argv[i]) == "--sink") {
      sink_name = argv[i + 1];
    } else if (std::string(argv[i]) == "--sink-device") {
      sink_device = argv[i + 1];
    } else if (std::string(argv[i]) == "--infer-timeout-ms") {
      infer_timeout_ms = parse_int(argv[i + 1], infer_timeout_ms);
    } else if (std::string(argv[i]) == "--events") {
      events_endpoint = argv[i + 1];
    } else if (std::string(argv[i]) == "--events-sync") {
      events_sync = argv[i + 1];
    }
  }
  if (events_endpoint.empty() != events_sync.empty()) {
    std::cerr << "--events 与 --events-sync 须成对指定" << std::endl;
    return 1;
  }
  if (backend_name != "fake" && backend_name != "melotts" &&
      backend_name != "summertts") {
    std::cerr << "未知后端: " << backend_name
              << "（支持 fake / melotts / summertts）" << std::endl;
    return 1;
  }
#ifdef SLOTNEXUS_HAS_MELOTTS
  if (backend_name == "melotts" && !melo.complete()) {
    std::cerr << "melotts 后端需要 --encoder-model/--decoder-model/--lexicon/"
                 "--tokens/--g-vector（或 session.json::tts 同名项）" << std::endl;
    return 1;
  }
#else
  if (backend_name == "melotts") {
    std::cerr << "当前构建未启用 MeloTTS 后端（需 "
                 "-DSLOTNEXUS_ENABLE_HARDWARE_BACKENDS=ON）" << std::endl;
    return 1;
  }
#endif
#ifdef SLOTNEXUS_HAS_SUMMERTTS
  if (backend_name == "summertts" && model_path.empty()) {
    std::cerr << "summertts 后端需要 --model（或 session.json::tts.model）" << std::endl;
    return 1;
  }
#else
  if (backend_name == "summertts") {
    std::cerr << "当前构建未启用 SummerTTS 后端（需 "
                 "-DSLOTNEXUS_ENABLE_HARDWARE_BACKENDS=ON）" << std::endl;
    return 1;
  }
#endif
  if (sink_name != "wav" && sink_name != "alsa") {
    std::cerr << "未知 sink: " << sink_name << "（支持 wav / alsa）"
              << std::endl;
    return 1;
  }
#ifdef SLOTNEXUS_HAS_ALSA
#else
  if (sink_name == "alsa") {
    std::cerr << "当前构建未启用 ALSA sink（需 "
                 "-DSLOTNEXUS_ENABLE_HARDWARE_BACKENDS=ON）" << std::endl;
    return 1;
  }
#endif

  // 后端工厂：每次 setup 产出独立实例（每任务一个模型上下文）。
  auto make_tts = [&]() -> std::unique_ptr<slotnexus::backend::ITtsBackend> {
    if (backend_name == "melotts") {
#ifdef SLOTNEXUS_HAS_MELOTTS
      slotnexus::backend::melotts::MeloTtsConfig config;
      config.encoder_model_path = melo.encoder_model;
      config.decoder_model_path = melo.decoder_model;
      config.lexicon_path = melo.lexicon;
      config.tokens_path = melo.tokens;
      config.g_path = melo.g_vector;
      config.speed = melo.speed;
      config.noise_scale = melo.noise_scale;
      config.noise_scale_w = melo.noise_scale_w;
      config.sdp_ratio = melo.sdp_ratio;
      config.intra_op_num_threads = melo.intra_op_threads;
      if (melo.native_sample_rate > 0) {
        config.native_sample_rate_hz =
            static_cast<std::uint32_t>(melo.native_sample_rate);
      }
      if (melo.max_encoder_phones > 0) {
        config.max_encoder_phones =
            static_cast<std::size_t>(melo.max_encoder_phones);
      }
      if (melo.run_timeout_ms > 0) {
        config.run_timeout_ms = static_cast<std::uint32_t>(melo.run_timeout_ms);
      }
      return std::make_unique<slotnexus::backend::melotts::MeloTtsBackend>(
          std::move(config));
#else
      throw std::runtime_error(
          "当前构建未启用 MeloTTS 后端（需 -DSLOTNEXUS_ENABLE_HARDWARE_BACKENDS=ON）");
#endif
    }
    if (backend_name == "summertts") {
#ifdef SLOTNEXUS_HAS_SUMMERTTS
      if (model_path.empty()) {
        throw std::runtime_error("summertts 后端需要 --model（或 session.json::tts.model）");
      }
      return std::make_unique<slotnexus::backend::summer_tts::SummerTtsBackend>(
          model_path, length_scale);
#else
      throw std::runtime_error(
          "当前构建未启用 SummerTTS 后端（需 -DSLOTNEXUS_ENABLE_HARDWARE_BACKENDS=ON）");
#endif
    }
    return std::make_unique<slotnexus::backend::fake::FakeTtsBackend>();
  };

  // sink 采样率 = 合成端实际输出率。SummerTTS 中文模型为 16 kHz（与 fake
  // 一致，契约 kSampleRateHz）；ALSA sink 在 ES8323 不支持该率时自动候选率
  // 回退 + 线性重采样（保音高保时长），WAV 路径 FakeAudioSink 同写 16 kHz 头。
  // （早期误用 22050——LJ Speech/VITS 经典率，中文模型非此；板端 F0 实测订正。）
  const int sink_sample_rate = slotnexus::backend::kSampleRateHz;

  // 启动时确保输出目录存在（仅 WAV 输出侧；alsa 模式不落盘）。
  if (sink_name == "wav") {
    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
      std::cerr << "tts_node 无法创建输出目录: " << output_dir << std::endl;
      return 1;
    }
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  zmq::context_t ctx(1);
  auto runtime = std::make_unique<slotnexus::runtime::TaskRuntime>(
      [make_tts, output_dir, sink_name, sink_device, sink_sample_rate] {
        return std::make_shared<TtsNodeBackend>(make_tts(), output_dir,
                                                sink_name, sink_device,
                                                sink_sample_rate);
      });
  // 数据面事件出口：--events 指定时绑定发布端点并注入节点外壳，
  // 推理中的 PCM 帧实时发布（订阅者先行握手，节点侧不阻塞等待）。
  std::shared_ptr<slotnexus::dataplane::EventPublisher> event_pub;
  if (!events_endpoint.empty()) {
    event_pub = std::make_shared<slotnexus::dataplane::EventPublisher>(ctx);
    event_pub->bind(events_endpoint, events_sync);
  }
  slotnexus::node::RuntimeNode node(ctx, std::move(runtime),
                                       std::chrono::milliseconds(infer_timeout_ms),
                                       event_pub);
  try {
    node.bind(listen);
    std::cout << "tts_node 监听 " << listen << "（" << backend_name << " 后端";
    if (backend_name == "melotts") {
      std::cout << "，编码器 " << melo.encoder_model << "，解码器 "
                << melo.decoder_model << "，语速 " << melo.speed;
    }
    if (backend_name == "summertts") {
      std::cout << "，模型 " << model_path << "，语速 " << length_scale;
    }
    std::cout << "，sink " << sink_name;
    if (sink_name == "alsa") {
      std::cout << "（" << sink_device << "，" << sink_sample_rate << " Hz）";
    } else {
      std::cout << "，输出目录 " << output_dir;
    }
    std::cout << "）" << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "tts_node 启动失败: " << e.what() << std::endl;
    return 1;
  }

  while (!g_stop) {
    node.serve_once(std::chrono::milliseconds(100));
  }
  node.close();
  std::cout << "tts_node 已退出" << std::endl;
  return 0;
}
