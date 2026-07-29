// tts_node 可执行入口：语音合成节点（WAV 输出）。
//
// 用法：tts_node [--listen tcp://127.0.0.1:19204] [--output-dir <目录>]
//                [--config <session.json>] [--backend fake|summertts]
//                [--model <模型路径>] [--length-scale <倍率>]
// 默认端口约定：echo 19200 / asr 19201 / rag 19202 / llm 19203 / tts 19204。
//
// Node 外壳（RuntimeNode + TaskRuntime）只依赖接口；本文件实现 IBackend
// 适配器，把流式 ITtsBackend 的 PCM 帧写入 FakeAudioSink（WAV 文件）：
//   - Mock 负载约定：客户端发 {"text": "<文本>"}；RuntimeNode 已提取 text
//     字段，适配器收到纯文本；
//   - 文件名 = <output-dir>/tts_<FNV-1a 文本哈希>.wav（确定性，相同文本
//     覆盖同名文件）；
//   - 返回 payload.text = JSON 字符串：{"wav_path": ..., "pcm_bytes": N}。
//
// 后端经工厂注入：--backend fake（默认，x86/Mock 回归基线）或 summertts
// （板端真实 vits，需 VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=ON 构建）。
// 模型路径经 --model 或 session.json::tts.model 参数化，不硬编码；
// 每次 setup 产出独立后端实例（TaskRuntime 工厂语义），SummerTTS 实例
// 持有独立模型上下文（峰值 RSS ~408 MB/实例，见 artifacts/upstream-baseline/）。
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

#include "voxorchestra/backend/backend_event.hpp"
#include "voxorchestra/backend/fake/fake_audio_sink.hpp"
#include "voxorchestra/backend/fake/fake_tts_backend.hpp"
#ifdef VOXORCHESTRA_HAS_SUMMERTTS
#include "voxorchestra/backend/summer_tts/summer_tts_backend.hpp"
#endif
#include "voxorchestra/runtime/ibackend.hpp"
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

// IBackend 适配器：合成 → 收集 PCM 块 → 写入 WAV 文件。
class TtsNodeBackend final : public voxorchestra::runtime::IBackend {
 public:
  // tts：后端实例（工厂注入，Fake / SummerTTS 可替换）。
  TtsNodeBackend(std::unique_ptr<voxorchestra::backend::ITtsBackend> tts,
                 std::string output_dir)
      : tts_(std::move(tts)), output_dir_(std::move(output_dir)) {}

  voxorchestra::runtime::BackendResult infer(
      const std::string& payload,
      std::chrono::steady_clock::time_point /*deadline*/,
      const std::atomic<bool>& cancelled) override {
    if (cancelled.load()) {
      tts_->cancel();
      return {voxorchestra::runtime::BackendResult::Code::kCancelled, {}};
    }
    std::vector<std::vector<int16_t>> chunks;
    tts_->set_event_callback([&chunks](const voxorchestra::backend::BackendEvent& e) {
      if (e.kind == voxorchestra::backend::BackendEvent::Kind::kPcm) {
        chunks.push_back(e.pcm);
      }
    });
    tts_->synthesize(payload);

    const std::string wav_path = output_dir_ + "/tts_" + TextHash(payload) + ".wav";
    voxorchestra::backend::fake::FakeAudioSink sink(wav_path);
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
    return {voxorchestra::runtime::BackendResult::Code::kOk, result.dump()};
  }

 private:
  std::unique_ptr<voxorchestra::backend::ITtsBackend> tts_;
  std::string output_dir_;
};

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int /*sig*/) { g_stop = 1; }

float parse_float(const char* s, float fallback) {
  try {
    return std::stof(s);
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
    }
  }
  if (backend_name != "fake" && backend_name != "summertts") {
    std::cerr << "未知后端: " << backend_name
              << "（支持 fake / summertts）" << std::endl;
    return 1;
  }
#ifdef VOXORCHESTRA_HAS_SUMMERTTS
  if (backend_name == "summertts" && model_path.empty()) {
    std::cerr << "summertts 后端需要 --model（或 session.json::tts.model）" << std::endl;
    return 1;
  }
#else
  if (backend_name == "summertts") {
    std::cerr << "当前构建未启用 SummerTTS 后端（需 "
                 "-DVOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=ON）" << std::endl;
    return 1;
  }
#endif

  // 后端工厂：每次 setup 产出独立实例（每任务一个模型上下文）。
  auto make_tts = [&]() -> std::unique_ptr<voxorchestra::backend::ITtsBackend> {
    if (backend_name == "summertts") {
#ifdef VOXORCHESTRA_HAS_SUMMERTTS
      if (model_path.empty()) {
        throw std::runtime_error("summertts 后端需要 --model（或 session.json::tts.model）");
      }
      return std::make_unique<voxorchestra::backend::summer_tts::SummerTtsBackend>(
          model_path, length_scale);
#else
      throw std::runtime_error(
          "当前构建未启用 SummerTTS 后端（需 -DVOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=ON）");
#endif
    }
    return std::make_unique<voxorchestra::backend::fake::FakeTtsBackend>();
  };

  // 启动时确保输出目录存在（Mock 输出侧）。
  std::error_code ec;
  std::filesystem::create_directories(output_dir, ec);
  if (ec) {
    std::cerr << "tts_node 无法创建输出目录: " << output_dir << std::endl;
    return 1;
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  zmq::context_t ctx(1);
  auto runtime = std::make_unique<voxorchestra::runtime::TaskRuntime>(
      [make_tts, output_dir] {
        return std::make_shared<TtsNodeBackend>(make_tts(), output_dir);
      });
  voxorchestra::node::RuntimeNode node(ctx, std::move(runtime));
  try {
    node.bind(listen);
    std::cout << "tts_node 监听 " << listen << "（" << backend_name << " 后端";
    if (backend_name == "summertts") {
      std::cout << "，模型 " << model_path << "，语速 " << length_scale;
    }
    std::cout << "，输出目录 " << output_dir << "）" << std::endl;
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
