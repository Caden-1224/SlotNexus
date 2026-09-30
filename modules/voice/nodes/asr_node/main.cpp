// asr_node 可执行入口：语音识别节点。
// Author: Caden
//
// 用法：asr_node [--listen tcp://127.0.0.1:19201] [--config <session.json>]
//                [--backend fake|sherpa_onnx] [--model <模型目录>]
//                [--model-precision fp32|int8] [--num-threads <n>]
//                [--infer-timeout-ms <ms>]
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
// （板端真实 ASR，需 SLOTNEXUS_ENABLE_HARDWARE_BACKENDS=ON 构建）。
// 模型目录经 --model 或 session.json::asr.model 参数化，不硬编码；
// 每次 setup 产出独立后端实例（TaskRuntime 工厂语义），sherpa-onnx 实例
// 持有独立识别器（模型上下文）。
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
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <zmq.hpp>

#include "slotnexus/backend/backend_event.hpp"
#include "slotnexus/backend/fake/fake_backends.hpp"
#include "slotnexus/backend/i_asr_backend.hpp"
#ifdef SLOTNEXUS_HAS_SHERTA_ONNX
#include "slotnexus/backend/sherpa_onnx/sherpa_asr_backend.hpp"
#endif
#include "slotnexus/common/wav_reader.hpp"
#include "slotnexus/transport/pushpull.hpp"
#include "slotnexus/runtime/ibackend.hpp"
#include "slotnexus/voice/event_adapter.hpp"
#include "runtime_node.hpp"

#include <algorithm>

namespace {

// IBackend 适配器：把流式 IAsrBackend 驱动到完成。
// 每帧间协作式检查 cancelled / deadline，命中即取消后端并尽快返回。
// 负载按后端约定解释：fake = 帧数（Mock），sherpa_onnx = WAV 路径。
class AsrNodeBackend final : public slotnexus::runtime::IBackend {
 public:
  using EventSink =
      std::function<void(const slotnexus::backend::BackendEvent&)>;

  // asr：后端实例（工厂注入，Fake / Sherpa 可替换）。
  // backend_name：驱动负载约定（fake / sherpa_onnx）。
  // fixture_dir：相对 WAV 路径的解析根（真实负载约定）。
  AsrNodeBackend(std::unique_ptr<slotnexus::backend::IAsrBackend> asr,
                 std::string backend_name, std::string fixture_dir)
      : asr_(std::move(asr)),
        backend_name_(std::move(backend_name)),
        fixture_dir_(std::move(fixture_dir)) {}

  slotnexus::runtime::BackendResult infer(
      const nlohmann::json& request,
      std::chrono::steady_clock::time_point deadline,
      const std::atomic<bool>& cancelled,
      const slotnexus::runtime::EventSink& events) override {
    std::string payload;
    if (request.is_object() && request.contains("text") &&
        request["text"].is_string()) {
      payload = request["text"].get<std::string>();
    } else {
      return {slotnexus::runtime::BackendResult::Code::kOk,
              {{"error", "asr 请求缺少字符串 payload.text"}}};
    }
    if (backend_name_ == "sherpa_onnx") {
      return run_wav(payload, deadline, cancelled, events);
    }
    return run_mock(payload, deadline, cancelled, events);
  }

  bool stream_start(const std::string& request_id,
                    EventSink sink) {
    if (!asr_) {
      return false;
    }
    stream_request_id_ = request_id;
    asr_->set_event_callback([sink = std::move(sink)](
                                 const slotnexus::backend::BackendEvent& e) {
      if (sink) {
        sink(e);
      }
    });
    return true;
  }

  bool stream_feed(const std::vector<std::int16_t>& pcm,
                   bool is_last) {
    if (!asr_) {
      return false;
    }
    asr_->feed_audio(pcm, is_last);
    return true;
  }

  void stream_cancel() {
    if (asr_) {
      asr_->cancel();
    }
  }

 private:
  // Mock 约定：payload 为提取后的纯文本 "<帧数>"；非法或非正数按 1 帧处理。
  slotnexus::runtime::BackendResult run_mock(
      const std::string& payload,
      std::chrono::steady_clock::time_point deadline,
      const std::atomic<bool>& cancelled,
      const slotnexus::runtime::EventSink& events) {
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
  slotnexus::runtime::BackendResult run_mock_frames(
      int frames, std::chrono::steady_clock::time_point deadline,
      const std::atomic<bool>& cancelled,
      const slotnexus::runtime::EventSink& events) {
    std::string final_text;
    asr_->set_event_callback(
        [&final_text, &events](const slotnexus::backend::BackendEvent& e) {
          if (e.kind == slotnexus::backend::BackendEvent::Kind::kFinal) {
            final_text = e.text;
          }
          if (events) {
            events(slotnexus::voice::ToRuntimeEvent(e));  // 语音事件适配为通用事件
          }
        });

    for (int i = 0; i < frames; ++i) {
      if (cancelled.load()) {
        asr_->cancel();
        return {slotnexus::runtime::BackendResult::Code::kCancelled, {}};
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        asr_->cancel();
        return {slotnexus::runtime::BackendResult::Code::kTimeout, {}};
      }
      asr_->feed_audio(slotnexus::backend::fake::FakeAudioSource::make_frame(
                           static_cast<std::uint32_t>(i)),
                       i + 1 == frames);
    }
    return {slotnexus::runtime::BackendResult::Code::kOk, {{"text", std::move(final_text)}}};
  }

  // 真实约定：payload 为 WAV 文件路径（相对路径按 fixture_dir 解析）。
  // WavReader 读全量采样 → 按 20 ms 帧（kFrameSamples=320）喂入 → kFinal 文本。
  slotnexus::runtime::BackendResult run_wav(
      const std::string& payload,
      std::chrono::steady_clock::time_point deadline,
      const std::atomic<bool>& cancelled,
      const slotnexus::runtime::EventSink& events) {
    std::string wav_path = payload;
    if (wav_path.empty()) {
      return {slotnexus::runtime::BackendResult::Code::kOk,
              {{"error", "负载为空：sherpa_onnx 后端需要 WAV 文件路径"}}};
    }
    if (wav_path[0] != '/' && !fixture_dir_.empty()) {
      wav_path = fixture_dir_ + "/" + wav_path;
    }
    const auto r = slotnexus::common::WavReader::read(wav_path);
    if (!r.ok) {
      return {slotnexus::runtime::BackendResult::Code::kOk,
              {{"error", "WAV 读取失败: " + r.error}}};
    }
    // 契约约定：16 kHz 单声道 16-bit PCM；不满足则拒绝（避免静默错识别）。
    if (r.info.sample_rate != slotnexus::backend::kSampleRateHz ||
        r.info.channels != slotnexus::backend::kChannels || r.info.bits != 16) {
      return {slotnexus::runtime::BackendResult::Code::kOk,
              {{"error", "WAV 格式不支持: " +
                             std::to_string(r.info.sample_rate) + " Hz/" +
                             std::to_string(r.info.channels) + "ch/" +
                             std::to_string(r.info.bits) +
                             "bit（需要 16000/1/16）"}}};
    }
    return feed_samples(r.info.samples, deadline, cancelled, events);
  }

  // 分块喂入样本（20 ms 帧，末帧 is_last）；协作式检查取消/超时。
  // 真实后端（sherpa_onnx）识别由会话侧累积的整段音频。
  slotnexus::runtime::BackendResult feed_samples(
      const std::vector<int16_t>& samples,
      std::chrono::steady_clock::time_point deadline,
      const std::atomic<bool>& cancelled,
      const slotnexus::runtime::EventSink& events) {
    std::string final_text;
    asr_->set_event_callback(
        [&final_text, &events](const slotnexus::backend::BackendEvent& e) {
          if (e.kind == slotnexus::backend::BackendEvent::Kind::kFinal) {
            final_text = e.text;
          }
          if (events) {
            events(slotnexus::voice::ToRuntimeEvent(e));  // 语音事件适配为通用事件
          }
        });
    std::size_t offset = 0;
    while (offset < samples.size()) {
      if (cancelled.load()) {
        asr_->cancel();
        return {slotnexus::runtime::BackendResult::Code::kCancelled, {}};
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        asr_->cancel();
        return {slotnexus::runtime::BackendResult::Code::kTimeout, {}};
      }
      const std::size_t n = std::min<std::size_t>(
          static_cast<std::size_t>(slotnexus::backend::kFrameSamples),
          samples.size() - offset);
      const std::vector<int16_t> frame(samples.begin() + offset,
                                       samples.begin() + offset + n);
      offset += n;
      asr_->feed_audio(frame, offset >= samples.size());
    }
    return {slotnexus::runtime::BackendResult::Code::kOk, {{"text", std::move(final_text)}}};
  }

  std::unique_ptr<slotnexus::backend::IAsrBackend> asr_;
  std::string backend_name_;
  std::string fixture_dir_;
  std::string stream_request_id_;
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
  std::string model_precision = "fp32";  // sherpa_onnx 精度：fp32 | int8
  int num_threads = 4;                // ONNX Runtime 线程数（门禁基线 4）
  int infer_timeout_ms = 0;           // 节点内推理超时；0 = 默认 5000 ms
  std::string fixture_dir;            // 相对 WAV 路径解析根
  std::string events_endpoint;        // 数据面事件 PUB 端点（可选）
  std::string events_sync;            // 配套握手端点
  std::string stream_endpoint;        // 流式 ASR 帧上行 PULL 端点（可选）

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
        model_precision = a.value("model_precision", model_precision);
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
    } else if (std::string(argv[i]) == "--model-precision") {
      model_precision = argv[i + 1];
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
    } else if (std::string(argv[i]) == "--stream") {
      stream_endpoint = argv[i + 1];
    }
  }
  if (events_endpoint.empty() != events_sync.empty()) {
    std::cerr << "--events 与 --events-sync 须成对指定" << std::endl;
    return 1;
  }
  if (!stream_endpoint.empty() && events_endpoint.empty()) {
    std::cerr << "--stream 需要同时指定 --events/--events-sync 以回传识别结果"
              << std::endl;
    return 1;
  }
  if (backend_name != "fake" && backend_name != "sherpa_onnx") {
    std::cerr << "未知后端: " << backend_name
              << "（支持 fake / sherpa_onnx）" << std::endl;
    return 1;
  }
#ifdef SLOTNEXUS_HAS_SHERTA_ONNX
  if (backend_name == "sherpa_onnx" && model_path.empty()) {
    std::cerr << "sherpa_onnx 后端需要 --model（或 session.json::asr.model）" << std::endl;
    return 1;
  }
  if (backend_name == "sherpa_onnx" && model_precision != "fp32" &&
      model_precision != "int8") {
    std::cerr << "未知 ASR 模型精度: " << model_precision
              << "（支持 fp32 / int8）" << std::endl;
    return 1;
  }
#else
  if (backend_name == "sherpa_onnx") {
    std::cerr << "当前构建未启用 sherpa-onnx 后端（需 "
                 "-DSLOTNEXUS_ENABLE_HARDWARE_BACKENDS=ON）" << std::endl;
    return 1;
  }
#endif

  // 后端工厂：每次 setup 产出独立实例（每任务一个识别器上下文）。
  auto make_asr = [&]() -> std::unique_ptr<slotnexus::backend::IAsrBackend> {
    if (backend_name == "sherpa_onnx") {
#ifdef SLOTNEXUS_HAS_SHERTA_ONNX
      if (model_path.empty()) {
        throw std::runtime_error("sherpa_onnx 后端需要 --model（或 session.json::asr.model）");
      }
      return std::make_unique<slotnexus::backend::sherpa_onnx::SherpaAsrBackend>(
          model_path, num_threads, model_precision);
#else
      throw std::runtime_error(
          "当前构建未启用 sherpa-onnx 后端（需 -DSLOTNEXUS_ENABLE_HARDWARE_BACKENDS=ON）");
#endif
    }
    return std::make_unique<slotnexus::backend::fake::FakeAsrBackend>();
  };

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  zmq::context_t ctx(1);
  auto runtime = std::make_unique<slotnexus::runtime::TaskRuntime>(
      [make_asr, backend_name, fixture_dir] {
        return std::make_shared<AsrNodeBackend>(make_asr(), backend_name,
                                                fixture_dir);
      });
  slotnexus::runtime::TaskRuntime* runtime_ptr = runtime.get();
  // 数据面事件出口：--events 指定时绑定发布端点并注入节点外壳，
  // 识别中间/最终文本实时发布（订阅者先行握手，节点侧不阻塞等待）。
  std::shared_ptr<slotnexus::dataplane::EventPublisher> event_pub;
  if (!events_endpoint.empty()) {
    event_pub = std::make_shared<slotnexus::dataplane::EventPublisher>(ctx);
    event_pub->bind(events_endpoint, events_sync);
  }

  // 流式 ASR 上行：独立 PULL 通道把 20 ms 帧直接喂给已 setup 的
  // AsrNodeBackend，而不是让每次推理在 RPC 负载里等整段音频。
  std::atomic<bool> stream_stop{false};
  std::unique_ptr<slotnexus::transport::PullSocket> stream_pull;
  std::thread stream_thread;
  if (!stream_endpoint.empty()) {
    stream_pull = std::make_unique<slotnexus::transport::PullSocket>(ctx);
    stream_pull->bind(stream_endpoint);
    stream_thread = std::thread([&] {
      const auto publish = [&](const slotnexus::runtime::BackendEvent& e,
                               const std::string& work_id,
                               const std::string& request_id) {
        slotnexus::dataplane::DataplaneEvent ev;
        ev.kind = e.type;
        ev.payload = e.payload;
        ev.finish = e.finish;
        event_pub->publish(ev, work_id, request_id);
      };
      while (!stream_stop.load()) {
        std::string raw;
        try {
          if (!stream_pull->recv(raw, std::chrono::milliseconds(100))) {
            continue;
          }
        } catch (const std::exception& e) {
          if (!stream_stop.load()) {
            std::cerr << "asr stream recv 失败: " << e.what() << std::endl;
          }
          break;
        }
        try {
          const auto msg = nlohmann::json::parse(raw);
          const std::string op = msg.value("op", std::string());
          const std::string work_id = msg.value("work_id", std::string());
          const std::string request_id =
              msg.value("request_id", std::string());
          auto base = runtime_ptr->find_backend(work_id);
          auto* stream = dynamic_cast<AsrNodeBackend*>(base.get());
          if (stream == nullptr) {
            std::cerr << "asr stream 未知 work_id=" << work_id << std::endl;
            continue;
          }
          if (op == "start") {
            stream->stream_start(
                request_id, [publish, work_id, request_id](
                                const slotnexus::backend::BackendEvent& e) {
                  publish(slotnexus::voice::ToRuntimeEvent(e), work_id,
                          request_id);
                });
          } else if (op == "frame") {
            const std::string b64 = msg.value("pcm", std::string());
            const auto bytes = slotnexus::common::base64_decode(b64);
            std::vector<std::int16_t> pcm(bytes.size() / sizeof(std::int16_t));
            if (!bytes.empty()) {
              std::memcpy(pcm.data(), bytes.data(), bytes.size());
            }
            stream->stream_feed(pcm, msg.value("last", false));
          } else if (op == "cancel") {
            stream->stream_cancel();
          } else {
            std::cerr << "asr stream 未知 op=" << op << std::endl;
          }
        } catch (const std::exception& e) {
          std::cerr << "asr stream 消息处理失败: " << e.what() << std::endl;
        }
      }
    });
  }

  slotnexus::node::RuntimeNode node(ctx, std::move(runtime),
                                       std::chrono::milliseconds(infer_timeout_ms),
                                       event_pub);
  try {
    node.bind(listen);
    std::cout << "asr_node 监听 " << listen << "（" << backend_name << " 后端";
    if (backend_name == "sherpa_onnx") {
      std::cout << "，模型 " << model_path << "，精度 " << model_precision
                << "，线程 " << num_threads;
    }
    if (!stream_endpoint.empty()) {
      std::cout << "，流式输入 " << stream_endpoint;
    }
    std::cout << "）" << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "asr_node 启动失败: " << e.what() << std::endl;
    return 1;
  }

  while (!g_stop) {
    node.serve_once(std::chrono::milliseconds(100));
  }
  stream_stop.store(true);
  if (stream_thread.joinable()) {
    stream_thread.join();
  }
  if (stream_pull) {
    stream_pull->close();
  }
  node.close();
  std::cout << "asr_node 已退出" << std::endl;
  return 0;
}
