// asr_node 可执行入口：语音识别节点。
//
// 用法：asr_node [--listen tcp://127.0.0.1:19201] [--config <session.json>]
//                [--backend fake|sherpa_onnx] [--model <模型目录>]
//                [--num-threads <n>] [--infer-timeout-ms <ms>]
//                [--fixture-dir <目录>]
// 默认端口约定：echo 19200 / asr 19201 / rag 19202 / llm 19203 / tts 19204。
//
// Node 外壳（RuntimeNode + TaskRuntime）只依赖接口；本文件实现 IBackend
// 适配器，把流式 IAsrBackend 驱动到完成并返回最终识别文本：
//   - Mock 负载约定（fake 后端）：客户端发 {"text": "<帧数N>"}；RuntimeNode
//     已提取 text 字段，适配器收到纯文本 "<N>"，用 FakeAudioSource 合成
//     N 帧确定性 PCM 送入 FakeAsrBackend；
//   - 真实负载约定（sherpa_onnx 后端）：payload 为 WAV 文件路径（相对路径
//     按 --fixture-dir 解析），读取后按 20 ms 帧送入，返回 kFinal 文本。
// 后端经工厂注入：--backend fake（默认，x86/Mock 回归基线）或 sherpa_onnx
// （板端真实 ASR，需 VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=ON 构建）。
// 模型目录经 --model 或 session.json::asr.model 参数化，不硬编码；
// 每次 setup 产出独立后端实例（TaskRuntime 工厂语义），sherpa-onnx 实例
// 持有独立识别器（模型上下文，见 artifacts/upstream-baseline/）。
// SIGINT/SIGTERM 优雅退出（退出码 0）。
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <zmq.hpp>

#include "voxorchestra/backend/backend_event.hpp"
#include "voxorchestra/backend/fake/fake_asr_backend.hpp"
#include "voxorchestra/backend/fake/fake_audio_source.hpp"
#include "voxorchestra/backend/i_asr_backend.hpp"
#ifdef VOXORCHESTRA_HAS_SHERTA_ONNX
#include "voxorchestra/backend/sherpa_onnx/sherpa_asr_backend.hpp"
#endif
#include "voxorchestra/common/base64.hpp"
#include "voxorchestra/common/wav_reader.hpp"
#include "voxorchestra/runtime/ibackend.hpp"
#include "runtime_node.hpp"

#include <algorithm>
#include <cstring>

namespace {

// IBackend 适配器：把流式 IAsrBackend 驱动到完成。
// 每帧间协作式检查 cancelled / deadline，命中即取消后端并尽快返回。
// 负载按后端约定解释：fake = 帧数（Mock），sherpa_onnx = WAV 路径。
class AsrNodeBackend final : public voxorchestra::runtime::IBackend {
 public:
  // asr：后端实例（工厂注入，Fake / Sherpa 可替换）。
  // backend_name：驱动负载约定（fake / sherpa_onnx）。
  // fixture_dir：相对 WAV 路径的解析根（真实负载约定）。
  AsrNodeBackend(std::unique_ptr<voxorchestra::backend::IAsrBackend> asr,
                 std::string backend_name, std::string fixture_dir)
      : asr_(std::move(asr)),
        backend_name_(std::move(backend_name)),
        fixture_dir_(std::move(fixture_dir)) {}

  voxorchestra::runtime::BackendResult infer(
      const std::string& payload,
      std::chrono::steady_clock::time_point deadline,
      const std::atomic<bool>& cancelled,
      const voxorchestra::runtime::EventSink& events) override {
    // 音频上行（net 会话真实负载）：会话侧把整段 PCM 编码上行。任何后端
    // 都先识别前缀（解码失败返回错误文本）；WAV 路径模式仅限节点直连。
    if (payload.rfind(voxorchestra::backend::kAsrPcmPayloadPrefix, 0) == 0) {
      return run_pcm(payload, deadline, cancelled, events);
    }
    if (backend_name_ == "sherpa_onnx") {
      return run_wav(payload, deadline, cancelled, events);
    }
    return run_mock(payload, deadline, cancelled, events);
  }

 private:
  // Mock 约定：payload 为提取后的纯文本 "<帧数>"；非法或非正数按 1 帧处理。
  voxorchestra::runtime::BackendResult run_mock(
      const std::string& payload,
      std::chrono::steady_clock::time_point deadline,
      const std::atomic<bool>& cancelled,
      const voxorchestra::runtime::EventSink& events) {
    int frames = 1;
    try {
      frames = std::stoi(payload);
      if (frames <= 0) {
        frames = 1;
      }
    } catch (...) {
      frames = 1;
    }
    return run_mock_frames(frames, deadline, cancelled, events);
  }

  // 帧数约定：合成 frames 帧确定性音频喂入，返回 kFinal 文本。
  voxorchestra::runtime::BackendResult run_mock_frames(
      int frames, std::chrono::steady_clock::time_point deadline,
      const std::atomic<bool>& cancelled,
      const voxorchestra::runtime::EventSink& events) {
    std::string final_text;
    asr_->set_event_callback(
        [&final_text, &events](const voxorchestra::backend::BackendEvent& e) {
          if (e.kind == voxorchestra::backend::BackendEvent::Kind::kFinal) {
            final_text = e.text;
          }
          if (events) {
            events(e);  // partial/final 实时转发数据面
          }
        });

    for (int i = 0; i < frames; ++i) {
      if (cancelled.load()) {
        asr_->cancel();
        return {voxorchestra::runtime::BackendResult::Code::kCancelled, {}};
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        asr_->cancel();
        return {voxorchestra::runtime::BackendResult::Code::kTimeout, {}};
      }
      asr_->feed_audio(voxorchestra::backend::fake::FakeAudioSource::make_frame(
                           static_cast<std::uint32_t>(i)),
                       i + 1 == frames);
    }
    return {voxorchestra::runtime::BackendResult::Code::kOk, std::move(final_text)};
  }

  // 音频上行约定：payload = "pcm64:<base64(16kHz/16bit/单声道 PCM)>"。
  // 解码后按后端解释：fake 按 20 ms 帧折算帧数（与帧数约定同语义，
  // 保证 Mock E2E 可测）；sherpa_onnx 直接分块喂真实后端。解码失败返回
  // 错误文本（与 run_wav 的负载错误约定一致）。
  voxorchestra::runtime::BackendResult run_pcm(
      const std::string& payload,
      std::chrono::steady_clock::time_point deadline,
      const std::atomic<bool>& cancelled,
      const voxorchestra::runtime::EventSink& events) {
    const std::string b64 = payload.substr(
        std::strlen(voxorchestra::backend::kAsrPcmPayloadPrefix));
    std::vector<std::uint8_t> bytes;
    try {
      bytes = voxorchestra::common::base64_decode(b64);
    } catch (const std::exception& e) {
      return {voxorchestra::runtime::BackendResult::Code::kOk,
              "{\"error\":\"PCM 负载解码失败: " + std::string(e.what()) + "\"}"};
    }
    if (bytes.empty() || bytes.size() % 2 != 0) {
      return {voxorchestra::runtime::BackendResult::Code::kOk,
              "{\"error\":\"PCM 负载字节数非法（需 16-bit 采样的偶数长度）\"}"};
    }
    std::vector<int16_t> samples(bytes.size() / 2);
    std::memcpy(samples.data(), bytes.data(), bytes.size());
    if (backend_name_ == "sherpa_onnx") {
      return feed_samples(samples, deadline, cancelled, events);
    }
    const std::size_t frames = std::max<std::size_t>(
        1, (samples.size() + voxorchestra::backend::kFrameSamples - 1) /
               voxorchestra::backend::kFrameSamples);
    return run_mock_frames(static_cast<int>(frames), deadline, cancelled,
                           events);
  }

  // 真实约定：payload 为 WAV 文件路径（相对路径按 fixture_dir 解析）。
  // WavReader 读全量采样 → 按 20 ms 帧（kFrameSamples=320）喂入 → kFinal 文本。
  voxorchestra::runtime::BackendResult run_wav(
      const std::string& payload,
      std::chrono::steady_clock::time_point deadline,
      const std::atomic<bool>& cancelled,
      const voxorchestra::runtime::EventSink& events) {
    std::string wav_path = payload;
    if (wav_path.empty()) {
      return {voxorchestra::runtime::BackendResult::Code::kOk,
              "{\"error\":\"负载为空：sherpa_onnx 后端需要 WAV 文件路径\"}"};
    }
    if (wav_path[0] != '/' && !fixture_dir_.empty()) {
      wav_path = fixture_dir_ + "/" + wav_path;
    }
    const auto r = voxorchestra::common::WavReader::read(wav_path);
    if (!r.ok) {
      return {voxorchestra::runtime::BackendResult::Code::kOk,
              "{\"error\":\"WAV 读取失败: " + r.error + "\"}"};
    }
    // 契约约定：16 kHz 单声道 16-bit PCM；不满足则拒绝（避免静默错识别）。
    if (r.info.sample_rate != voxorchestra::backend::kSampleRateHz ||
        r.info.channels != voxorchestra::backend::kChannels || r.info.bits != 16) {
      return {voxorchestra::runtime::BackendResult::Code::kOk,
              "{\"error\":\"WAV 格式不支持: " + std::to_string(r.info.sample_rate) +
                  " Hz/" + std::to_string(r.info.channels) + "ch/" +
                  std::to_string(r.info.bits) + "bit（需要 16000/1/16）\"}"};
    }
    return feed_samples(r.info.samples, deadline, cancelled, events);
  }

  // 分块喂入样本（20 ms 帧，末帧 is_last）；协作式检查取消/超时。
  // 真实后端（sherpa_onnx）识别由会话侧累积的整段音频。
  voxorchestra::runtime::BackendResult feed_samples(
      const std::vector<int16_t>& samples,
      std::chrono::steady_clock::time_point deadline,
      const std::atomic<bool>& cancelled,
      const voxorchestra::runtime::EventSink& events) {
    std::string final_text;
    asr_->set_event_callback(
        [&final_text, &events](const voxorchestra::backend::BackendEvent& e) {
          if (e.kind == voxorchestra::backend::BackendEvent::Kind::kFinal) {
            final_text = e.text;
          }
          if (events) {
            events(e);  // partial/final 实时转发数据面
          }
        });
    std::size_t offset = 0;
    while (offset < samples.size()) {
      if (cancelled.load()) {
        asr_->cancel();
        return {voxorchestra::runtime::BackendResult::Code::kCancelled, {}};
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        asr_->cancel();
        return {voxorchestra::runtime::BackendResult::Code::kTimeout, {}};
      }
      const std::size_t n = std::min<std::size_t>(
          static_cast<std::size_t>(voxorchestra::backend::kFrameSamples),
          samples.size() - offset);
      const std::vector<int16_t> frame(samples.begin() + offset,
                                       samples.begin() + offset + n);
      offset += n;
      asr_->feed_audio(frame, offset >= samples.size());
    }
    return {voxorchestra::runtime::BackendResult::Code::kOk, std::move(final_text)};
  }

  std::unique_ptr<voxorchestra::backend::IAsrBackend> asr_;
  std::string backend_name_;
  std::string fixture_dir_;
};

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int /*sig*/) { g_stop = 1; }

int parse_int(const char* s, int fallback) {
  try {
    return std::stoi(s);
  } catch (...) {
    return fallback;
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string listen = "tcp://127.0.0.1:19201";
  std::string backend_name = "fake";  // 默认 Fake（x86/Mock 回归基线）
  std::string model_path;             // sherpa_onnx 后端必填（模型目录）
  int num_threads = 4;                // ONNX Runtime 线程数（门禁基线 4）
  int infer_timeout_ms = 0;           // 节点内推理超时；0 = 默认 5000 ms
  std::string fixture_dir;            // 相对 WAV 路径解析根
  std::string events_endpoint;        // 数据面事件 PUB 端点（可选）
  std::string events_sync;            // 配套握手端点

  // 先读配置文件（--config 的 asr 段），命令行参数随后覆盖。
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
      if (file_cfg.contains("asr")) {
        const auto& a = file_cfg["asr"];
        backend_name = a.value("backend", backend_name);
        model_path = a.value("model", model_path);
        num_threads = a.value("num_threads", num_threads);
        fixture_dir = a.value("fixture_dir", fixture_dir);
      }
    }
  }
  for (int i = 1; i < argc - 1; ++i) {
    if (std::string(argv[i]) == "--listen") {
      listen = argv[i + 1];
    } else if (std::string(argv[i]) == "--backend") {
      backend_name = argv[i + 1];
    } else if (std::string(argv[i]) == "--model") {
      model_path = argv[i + 1];
    } else if (std::string(argv[i]) == "--num-threads") {
      num_threads = parse_int(argv[i + 1], num_threads);
    } else if (std::string(argv[i]) == "--infer-timeout-ms") {
      infer_timeout_ms = parse_int(argv[i + 1], infer_timeout_ms);
    } else if (std::string(argv[i]) == "--fixture-dir") {
      fixture_dir = argv[i + 1];
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
  if (backend_name != "fake" && backend_name != "sherpa_onnx") {
    std::cerr << "未知后端: " << backend_name
              << "（支持 fake / sherpa_onnx）" << std::endl;
    return 1;
  }
#ifdef VOXORCHESTRA_HAS_SHERTA_ONNX
  if (backend_name == "sherpa_onnx" && model_path.empty()) {
    std::cerr << "sherpa_onnx 后端需要 --model（或 session.json::asr.model）" << std::endl;
    return 1;
  }
#else
  if (backend_name == "sherpa_onnx") {
    std::cerr << "当前构建未启用 sherpa-onnx 后端（需 "
                 "-DVOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=ON）" << std::endl;
    return 1;
  }
#endif

  // 后端工厂：每次 setup 产出独立实例（每任务一个识别器上下文）。
  auto make_asr = [&]() -> std::unique_ptr<voxorchestra::backend::IAsrBackend> {
    if (backend_name == "sherpa_onnx") {
#ifdef VOXORCHESTRA_HAS_SHERTA_ONNX
      if (model_path.empty()) {
        throw std::runtime_error("sherpa_onnx 后端需要 --model（或 session.json::asr.model）");
      }
      return std::make_unique<voxorchestra::backend::sherpa_onnx::SherpaAsrBackend>(
          model_path, num_threads);
#else
      throw std::runtime_error(
          "当前构建未启用 sherpa-onnx 后端（需 -DVOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=ON）");
#endif
    }
    return std::make_unique<voxorchestra::backend::fake::FakeAsrBackend>();
  };

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  zmq::context_t ctx(1);
  auto runtime = std::make_unique<voxorchestra::runtime::TaskRuntime>(
      [make_asr, backend_name, fixture_dir] {
        return std::make_shared<AsrNodeBackend>(make_asr(), backend_name,
                                                fixture_dir);
      });
  // 数据面事件出口：--events 指定时绑定发布端点并注入节点外壳，
  // 识别中间/最终文本实时发布（订阅者先行握手，节点侧不阻塞等待）。
  std::shared_ptr<voxorchestra::dataplane::EventPublisher> event_pub;
  if (!events_endpoint.empty()) {
    event_pub = std::make_shared<voxorchestra::dataplane::EventPublisher>(ctx);
    event_pub->bind(events_endpoint, events_sync);
  }
  voxorchestra::node::RuntimeNode node(ctx, std::move(runtime),
                                       std::chrono::milliseconds(infer_timeout_ms),
                                       event_pub);
  try {
    node.bind(listen);
    std::cout << "asr_node 监听 " << listen << "（" << backend_name << " 后端";
    if (backend_name == "sherpa_onnx") {
      std::cout << "，模型 " << model_path << "，线程 " << num_threads;
    }
    std::cout << "）" << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "asr_node 启动失败: " << e.what() << std::endl;
    return 1;
  }

  while (!g_stop) {
    node.serve_once(std::chrono::milliseconds(100));
  }
  node.close();
  std::cout << "asr_node 已退出" << std::endl;
  return 0;
}
