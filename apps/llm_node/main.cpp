// llm_node 可执行入口：大模型文本生成节点。
//
// 用法：llm_node [--listen tcp://127.0.0.1:19203] [--config <session.json>]
//                [--backend fake|rkllm] [--model <模型文件>]
//                [--max-new-tokens <n>] [--max-context-len <n>]
//                [--infer-timeout-ms <ms>]
// 默认端口约定：echo 19200 / asr 19201 / rag 19202 / llm 19203 / tts 19204。
//
// Node 外壳（RuntimeNode + TaskRuntime）只依赖接口；本文件实现 IBackend
// 适配器，把流式 ILlmBackend 驱动到完成并返回最终文本：
//   - Mock 负载约定（fake 后端）：客户端发 {"text": "<prompt>"}；RuntimeNode
//     已提取 text 字段，适配器收到纯文本 prompt，同步生成（瞬时）；
//   - 真实负载约定（rkllm 后端）：payload 为纯文本 prompt；单次生成可能
//     数秒（1.5B W4A16 板端 ~7.8 tok/s，见 artifacts/upstream-baseline/），
//     生成在后台线程执行，主线程轮询 cancelled / deadline，命中即取消
//     后端并尽快返回（控制面 RPC 超时由 --forward-timeout-ms /
//     --node-rpc-timeout-ms 参数化，默认 3000 ms）。
// 后端经工厂注入：--backend fake（默认，x86/Mock 回归基线）或 rkllm
// （板端真实大模型，需 VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=ON 构建）。
// 模型路径经 --model 或 session.json::llm.model 参数化，不硬编码；
// 每次 setup 产出独立后端实例（TaskRuntime 工厂语义），rkllm 实例持有
// 独立模型上下文（加载耗时在 setup 路径内）。
// SIGINT/SIGTERM 优雅退出（退出码 0）。
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>
#include <zmq.hpp>

#include "voxorchestra/backend/backend_event.hpp"
#include "voxorchestra/backend/fake/fake_llm_backend.hpp"
#include "voxorchestra/backend/i_llm_backend.hpp"
#ifdef VOXORCHESTRA_HAS_RKLLM
#include "voxorchestra/backend/rkllm/rkllm_llm_backend.hpp"
#endif
#include "voxorchestra/runtime/ibackend.hpp"
#include "runtime_node.hpp"

namespace {

// IBackend 适配器：把流式 ILlmBackend 驱动到完成。
// 负载按后端约定解释：fake / rkllm 均为纯文本 prompt（Mock 负载约定）；
// rkllm 单次生成耗时数秒，在后台线程执行并协作式响应 cancelled / deadline。
class LlmNodeBackend final : public voxorchestra::runtime::IBackend {
 public:
  // llm：后端实例（工厂注入，Fake / Rkllm 可替换）。
  // backend_name：驱动负载约定（fake / rkllm）。
  LlmNodeBackend(std::unique_ptr<voxorchestra::backend::ILlmBackend> llm,
                 std::string backend_name)
      : llm_(std::move(llm)), backend_name_(std::move(backend_name)) {}

  voxorchestra::runtime::BackendResult infer(
      const std::string& payload,
      std::chrono::steady_clock::time_point deadline,
      const std::atomic<bool>& cancelled,
      const voxorchestra::runtime::EventSink& events) override {
    if (cancelled.load()) {
      llm_->cancel();
      return {voxorchestra::runtime::BackendResult::Code::kCancelled, {}};
    }
    if (backend_name_ == "rkllm") {
      return run_rkllm(payload, deadline, cancelled, events);
    }
    return run_fake(payload, events);
  }

 private:
  // Mock 约定：payload 为提取后的纯文本 prompt，同步生成（Fake 瞬时）。
  voxorchestra::runtime::BackendResult run_fake(
      const std::string& payload,
      const voxorchestra::runtime::EventSink& events) {
    std::string final_text;
    llm_->set_event_callback(
        [&final_text, &events](const voxorchestra::backend::BackendEvent& e) {
          if (e.kind == voxorchestra::backend::BackendEvent::Kind::kDone) {
            final_text = e.text;
          }
          if (events) {
            events(e);  // token/done 实时转发数据面
          }
        });
    llm_->generate(payload);
    return {voxorchestra::runtime::BackendResult::Code::kOk, std::move(final_text)};
  }

  // 真实约定：payload 为纯文本 prompt。生成在后台线程执行（数秒级，
  // 事件回调只在该线程被调用）；主线程轮询 cancelled / deadline，命中即
  // llm_->cancel()（后端过滤旧 token）后 join 返回。
  voxorchestra::runtime::BackendResult run_rkllm(
      const std::string& payload,
      std::chrono::steady_clock::time_point deadline,
      const std::atomic<bool>& cancelled,
      const voxorchestra::runtime::EventSink& events) {
    std::string final_text;
    llm_->set_event_callback(
        [&final_text, &events](const voxorchestra::backend::BackendEvent& e) {
          if (e.kind == voxorchestra::backend::BackendEvent::Kind::kDone) {
            final_text = e.text;
          }
          if (events) {
            events(e);  // token/done 实时转发数据面
          }
        });
    std::atomic<bool> gen_done{false};
    std::thread worker([this, &payload, &final_text, &gen_done] {
      llm_->generate(payload);
      gen_done.store(true);
    });
    while (!gen_done.load()) {
      if (cancelled.load()) {
        llm_->cancel();
        worker.join();
        return {voxorchestra::runtime::BackendResult::Code::kCancelled, {}};
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        llm_->cancel();
        worker.join();
        return {voxorchestra::runtime::BackendResult::Code::kTimeout, {}};
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    worker.join();
    return {voxorchestra::runtime::BackendResult::Code::kOk, std::move(final_text)};
  }

  std::unique_ptr<voxorchestra::backend::ILlmBackend> llm_;
  std::string backend_name_;
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
  std::string listen = "tcp://127.0.0.1:19203";
  std::string backend_name = "fake";  // 默认 Fake（x86/Mock 回归基线）
  std::string model_path;             // rkllm 后端必填（.rkllm 模型文件）
  int max_new_tokens = 100;           // 采样参数（教程参考值，板端实测校准）
  int max_context_len = 256;
  int infer_timeout_ms = 0;           // 节点内推理超时；0 = 默认 5000 ms
  std::string events_endpoint;        // 数据面事件 PUB 端点（可选）
  std::string events_sync;            // 配套握手端点

  // 先读配置文件（--config 的 llm 段），命令行参数随后覆盖。
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
      if (file_cfg.contains("llm")) {
        const auto& l = file_cfg["llm"];
        backend_name = l.value("backend", backend_name);
        model_path = l.value("model", model_path);
        max_new_tokens = l.value("max_new_tokens", max_new_tokens);
        max_context_len = l.value("max_context_len", max_context_len);
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
    } else if (std::string(argv[i]) == "--max-new-tokens") {
      max_new_tokens = parse_int(argv[i + 1], max_new_tokens);
    } else if (std::string(argv[i]) == "--max-context-len") {
      max_context_len = parse_int(argv[i + 1], max_context_len);
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
  if (backend_name != "fake" && backend_name != "rkllm") {
    std::cerr << "未知后端: " << backend_name
              << "（支持 fake / rkllm）" << std::endl;
    return 1;
  }
#ifdef VOXORCHESTRA_HAS_RKLLM
  if (backend_name == "rkllm" && model_path.empty()) {
    std::cerr << "rkllm 后端需要 --model（或 session.json::llm.model）" << std::endl;
    return 1;
  }
#else
  if (backend_name == "rkllm") {
    std::cerr << "当前构建未启用 rkllm 后端（需 "
                 "-DVOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=ON）" << std::endl;
    return 1;
  }
#endif

  // 后端工厂：每次 setup 产出独立实例（每任务一个模型上下文）。
  auto make_llm = [&]() -> std::unique_ptr<voxorchestra::backend::ILlmBackend> {
    if (backend_name == "rkllm") {
#ifdef VOXORCHESTRA_HAS_RKLLM
      if (model_path.empty()) {
        throw std::runtime_error("rkllm 后端需要 --model（或 session.json::llm.model）");
      }
      return std::make_unique<voxorchestra::backend::rkllm::RkllmBackend>(
          model_path, max_new_tokens, max_context_len);
#else
      throw std::runtime_error(
          "当前构建未启用 rkllm 后端（需 -DVOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=ON）");
#endif
    }
    return std::make_unique<voxorchestra::backend::fake::FakeLlmBackend>();
  };

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  zmq::context_t ctx(1);
  auto runtime = std::make_unique<voxorchestra::runtime::TaskRuntime>(
      [make_llm, backend_name] {
        return std::make_shared<LlmNodeBackend>(make_llm(), backend_name);
      });
  // 数据面事件出口：--events 指定时绑定发布端点并注入节点外壳，
  // 生成 token/done 实时发布（订阅者先行握手，节点侧不阻塞等待）。
  std::shared_ptr<voxorchestra::dataplane::EventPublisher> event_pub;
  if (!events_endpoint.empty()) {
    event_pub = std::make_shared<voxorchestra::dataplane::EventPublisher>(ctx);
    event_pub->bind(events_endpoint, events_sync);
  }
  voxorchestra::node::RuntimeNode node(
      ctx, std::move(runtime),
      std::chrono::milliseconds(infer_timeout_ms), event_pub);
  try {
    node.bind(listen);
    std::cout << "llm_node 监听 " << listen << "（" << backend_name << " 后端";
    if (backend_name == "rkllm") {
      std::cout << "，模型 " << model_path << "，max_new_tokens " << max_new_tokens
                << " / max_context_len " << max_context_len;
    }
    std::cout << "）" << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "llm_node 启动失败: " << e.what() << std::endl;
    return 1;
  }

  while (!g_stop) {
    node.serve_once(std::chrono::milliseconds(100));
  }
  node.close();
  std::cout << "llm_node 已退出" << std::endl;
  return 0;
}
