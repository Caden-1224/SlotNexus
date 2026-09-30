// llm_node 可执行入口：大模型文本生成节点。
// Author: Caden
//
// 用法：llm_node [--listen tcp://127.0.0.1:19203] [--config <session.json>]
//                [--backend fake|rkllm] [--model <模型文件>]
//                [--max-new-tokens <n>] [--max-context-len <n>]
//                [--top-k <n>] [--top-p <f>] [--temperature <f>]
//                [--repeat-penalty <f>] [--frequency-penalty <f>]
//                [--presence-penalty <f>] [--skip-special-token <0|1>]
//                [--ignore-eos-token <0|1>] [--enable-thinking <0|1>]
//                [--reasoning-end-tag <tag>] [--cpus-num <n>] [--cpus-mask <n>]
//                [--infer-timeout-ms <ms>]
// 默认端口约定：echo 19200 / asr 19201 / rag 19202 / llm 19203 / tts 19204。
//
// Node 外壳（RuntimeNode + TaskRuntime）只依赖接口；本文件实现 IBackend
// 适配器，把流式 ILlmBackend 驱动到完成并返回最终文本：
//   - Mock 负载约定（fake 后端）：客户端发 {"text": "<prompt>"}；RuntimeNode
//     已提取 text 字段，适配器收到纯文本 prompt，同步生成（瞬时）；
//   - 真实负载约定（rkllm 后端）：payload 为纯文本 prompt；单次生成可能
//     数秒（Qwen3.5-0.8B W4A16 板端 TTFT 约 3 s、约 7 tok/s），
//     生成在后台线程执行，主线程轮询 cancelled / deadline，命中即取消
//     后端并尽快返回（控制面 RPC 超时由 --forward-timeout-ms /
//     --node-rpc-timeout-ms 参数化，默认 3000 ms）。
// 后端经工厂注入：--backend fake（默认，x86/Mock 回归基线）或 rkllm
// （板端真实大模型，需 SLOTNEXUS_ENABLE_HARDWARE_BACKENDS=ON 构建）。
// 模型路径、采样参数、思考模式开关与思考段过滤标记经 --model/--top-k/…
// 或 session.json::llm.* 参数化，代码内不保留任何单一模型的硬编码假设
// （见 rkllm_options.hpp）；每次 setup 产出独立后端实例（TaskRuntime 工厂
// 语义），rkllm 实例持有独立模型上下文（加载耗时在 setup 路径内）。
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

#include "slotnexus/backend/backend_event.hpp"
#include "slotnexus/backend/fake/fake_backends.hpp"
#include "slotnexus/backend/i_llm_backend.hpp"
// 选项与校验不依赖厂商 SDK：默认（无硬件）构建同样编译本文件，因此非法
// 配置在启动阶段即可快速失败，而不是等到板端 rkllm_init。
#include "slotnexus/backend/rkllm/rkllm_options.hpp"
#ifdef SLOTNEXUS_HAS_RKLLM
#include "slotnexus/backend/rkllm/rkllm_llm_backend.hpp"
#endif
#include "slotnexus/runtime/ibackend.hpp"
#include "slotnexus/voice/event_adapter.hpp"
#include "runtime_node.hpp"

namespace {

// IBackend 适配器：把流式 ILlmBackend 驱动到完成。
// 负载按后端约定解释：fake / rkllm 均为纯文本 prompt（Mock 负载约定）；
// rkllm 单次生成耗时数秒，在后台线程执行并协作式响应 cancelled / deadline。
class LlmNodeBackend final : public slotnexus::runtime::IBackend {
 public:
  // llm：后端实例（工厂注入，Fake / Rkllm 可替换）。
  // backend_name：驱动负载约定（fake / rkllm）。
  LlmNodeBackend(std::unique_ptr<slotnexus::backend::ILlmBackend> llm,
                 std::string backend_name)
      : llm_(std::move(llm)), backend_name_(std::move(backend_name)) {}

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
              {{"error", "llm 请求缺少字符串 payload.text"}}};
    }
    if (cancelled.load()) {
      llm_->cancel();
      return {slotnexus::runtime::BackendResult::Code::kCancelled, {}};
    }
    if (backend_name_ == "rkllm") {
      return run_rkllm(payload, deadline, cancelled, events);
    }
    return run_fake(payload, events);
  }

 private:
  // Mock 约定：payload 为提取后的纯文本 prompt，同步生成（Fake 瞬时）。
  slotnexus::runtime::BackendResult run_fake(
      const std::string& payload,
      const slotnexus::runtime::EventSink& events) {
    std::string final_text;
    llm_->set_event_callback(
        [&final_text, &events](const slotnexus::backend::BackendEvent& e) {
          if (e.kind == slotnexus::backend::BackendEvent::Kind::kDone) {
            final_text = e.text;
          }
          if (events) {
            events(slotnexus::voice::ToRuntimeEvent(e));  // 语音事件适配为通用事件
          }
        });
    llm_->generate(payload);
    return {slotnexus::runtime::BackendResult::Code::kOk, {{"text", std::move(final_text)}}};
  }

  // 真实约定：payload 为纯文本 prompt。生成在后台线程执行（数秒级，
  // 事件回调只在该线程被调用）；主线程轮询 cancelled / deadline，命中即
  // llm_->cancel()（后端过滤旧 token）后 join 返回。
  slotnexus::runtime::BackendResult run_rkllm(
      const std::string& payload,
      std::chrono::steady_clock::time_point deadline,
      const std::atomic<bool>& cancelled,
      const slotnexus::runtime::EventSink& events) {
    std::string final_text;
    llm_->set_event_callback(
        [&final_text, &events](const slotnexus::backend::BackendEvent& e) {
          if (e.kind == slotnexus::backend::BackendEvent::Kind::kDone) {
            final_text = e.text;
          }
          if (events) {
            events(slotnexus::voice::ToRuntimeEvent(e));  // 语音事件适配为通用事件
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
        return {slotnexus::runtime::BackendResult::Code::kCancelled, {}};
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        llm_->cancel();
        worker.join();
        return {slotnexus::runtime::BackendResult::Code::kTimeout, {}};
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    worker.join();
    return {slotnexus::runtime::BackendResult::Code::kOk, {{"text", std::move(final_text)}}};
  }

  std::unique_ptr<slotnexus::backend::ILlmBackend> llm_;
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

float parse_float(const char* s, float fallback) {
  try {
    return std::stof(s);
  } catch (...) {
    return fallback;
  }
}

unsigned int parse_uint(const char* s, unsigned int fallback) {
  try {
    const unsigned long v = std::stoul(s);
    if (v > 0xFFFFFFFFul) {
      return fallback;
    }
    return static_cast<unsigned int>(v);
  } catch (...) {
    return fallback;
  }
}

// 布尔参数只接受 1/0/true/false；其他取值保持原值并告警，避免把拼错的参数
// 静默当成 false 生效。
bool parse_bool(const char* s, bool fallback) {
  const std::string v(s);
  if (v == "1" || v == "true" || v == "True" || v == "TRUE") {
    return true;
  }
  if (v == "0" || v == "false" || v == "False" || v == "FALSE") {
    return false;
  }
  std::cerr << "布尔参数取值非法（" << v << "），保持原值" << std::endl;
  return fallback;
}

}  // namespace

int main(int argc, char** argv) {
  std::string listen = "tcp://127.0.0.1:19203";
  std::string backend_name = "fake";  // 默认 Fake（x86/Mock 回归基线）
  // rkllm 后端的全部可配置项（模型路径 + 采样 + 思考模式 + 运行参数）。
  // 默认值 = 改造前硬编码值，因此不写这些键时行为与旧版本一致。
  slotnexus::backend::rkllm::RkllmOptions llm_options;
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
        llm_options.model_path =
            l.value("model", llm_options.model_path);
        llm_options.max_new_tokens =
            l.value("max_new_tokens", llm_options.max_new_tokens);
        llm_options.max_context_len =
            l.value("max_context_len", llm_options.max_context_len);
        llm_options.top_k = l.value("top_k", llm_options.top_k);
        llm_options.top_p = l.value("top_p", llm_options.top_p);
        llm_options.temperature =
            l.value("temperature", llm_options.temperature);
        llm_options.repeat_penalty =
            l.value("repeat_penalty", llm_options.repeat_penalty);
        llm_options.frequency_penalty =
            l.value("frequency_penalty", llm_options.frequency_penalty);
        llm_options.presence_penalty =
            l.value("presence_penalty", llm_options.presence_penalty);
        llm_options.skip_special_token =
            l.value("skip_special_token", llm_options.skip_special_token);
        llm_options.ignore_eos_token =
            l.value("ignore_eos_token", llm_options.ignore_eos_token);
        llm_options.enable_thinking =
            l.value("enable_thinking", llm_options.enable_thinking);
        llm_options.reasoning_end_tag =
            l.value("reasoning_end_tag", llm_options.reasoning_end_tag);
        llm_options.reasoning_max_buffer_bytes =
            l.value("reasoning_max_buffer_bytes",
                    llm_options.reasoning_max_buffer_bytes);
        llm_options.enabled_cpus_num =
            l.value("enabled_cpus_num", llm_options.enabled_cpus_num);
        llm_options.enabled_cpus_mask =
            l.value("enabled_cpus_mask", llm_options.enabled_cpus_mask);
        llm_options.embed_flash =
            l.value("embed_flash", llm_options.embed_flash);
        llm_options.base_domain_id =
            l.value("base_domain_id", llm_options.base_domain_id);
      }
    }
  }
  for (int i = 1; i < argc - 1; ++i) {
    if (std::string(argv[i]) == "--listen") {
      listen = argv[i + 1];
    } else if (std::string(argv[i]) == "--backend") {
      backend_name = argv[i + 1];
    } else if (std::string(argv[i]) == "--model") {
      llm_options.model_path = argv[i + 1];
    } else if (std::string(argv[i]) == "--max-new-tokens") {
      llm_options.max_new_tokens =
          parse_int(argv[i + 1], llm_options.max_new_tokens);
    } else if (std::string(argv[i]) == "--max-context-len") {
      llm_options.max_context_len =
          parse_int(argv[i + 1], llm_options.max_context_len);
    } else if (std::string(argv[i]) == "--top-k") {
      llm_options.top_k = parse_int(argv[i + 1], llm_options.top_k);
    } else if (std::string(argv[i]) == "--top-p") {
      llm_options.top_p = parse_float(argv[i + 1], llm_options.top_p);
    } else if (std::string(argv[i]) == "--temperature") {
      llm_options.temperature =
          parse_float(argv[i + 1], llm_options.temperature);
    } else if (std::string(argv[i]) == "--repeat-penalty") {
      llm_options.repeat_penalty =
          parse_float(argv[i + 1], llm_options.repeat_penalty);
    } else if (std::string(argv[i]) == "--frequency-penalty") {
      llm_options.frequency_penalty =
          parse_float(argv[i + 1], llm_options.frequency_penalty);
    } else if (std::string(argv[i]) == "--presence-penalty") {
      llm_options.presence_penalty =
          parse_float(argv[i + 1], llm_options.presence_penalty);
    } else if (std::string(argv[i]) == "--skip-special-token") {
      llm_options.skip_special_token =
          parse_bool(argv[i + 1], llm_options.skip_special_token);
    } else if (std::string(argv[i]) == "--ignore-eos-token") {
      llm_options.ignore_eos_token =
          parse_bool(argv[i + 1], llm_options.ignore_eos_token);
    } else if (std::string(argv[i]) == "--enable-thinking") {
      llm_options.enable_thinking =
          parse_bool(argv[i + 1], llm_options.enable_thinking);
    } else if (std::string(argv[i]) == "--reasoning-end-tag") {
      llm_options.reasoning_end_tag = argv[i + 1];
    } else if (std::string(argv[i]) == "--cpus-num") {
      llm_options.enabled_cpus_num =
          parse_int(argv[i + 1], llm_options.enabled_cpus_num);
    } else if (std::string(argv[i]) == "--cpus-mask") {
      llm_options.enabled_cpus_mask =
          parse_uint(argv[i + 1], llm_options.enabled_cpus_mask);
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
  if (backend_name == "rkllm") {
    // 采样/运行参数在启动阶段校验（不依赖厂商 SDK），避免把非法参数带进
    // rkllm_init 之后才在 setup 路径失败。
    const std::string reason =
        slotnexus::backend::rkllm::validate(llm_options);
    if (!reason.empty()) {
      std::cerr << "llm 配置非法: " << reason << std::endl;
      return 1;
    }
  }
#ifndef SLOTNEXUS_HAS_RKLLM
  if (backend_name == "rkllm") {
    std::cerr << "当前构建未启用 rkllm 后端（需 "
                 "-DSLOTNEXUS_ENABLE_HARDWARE_BACKENDS=ON）" << std::endl;
    return 1;
  }
#endif

  // 后端工厂：每次 setup 产出独立实例（每任务一个模型上下文）。
  auto make_llm = [&]() -> std::unique_ptr<slotnexus::backend::ILlmBackend> {
    if (backend_name == "rkllm") {
#ifdef SLOTNEXUS_HAS_RKLLM
      return std::make_unique<slotnexus::backend::rkllm::RkllmBackend>(
          llm_options);
#else
      throw std::runtime_error(
          "当前构建未启用 rkllm 后端（需 -DSLOTNEXUS_ENABLE_HARDWARE_BACKENDS=ON）");
#endif
    }
    return std::make_unique<slotnexus::backend::fake::FakeLlmBackend>();
  };

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  zmq::context_t ctx(1);
  auto runtime = std::make_unique<slotnexus::runtime::TaskRuntime>(
      [make_llm, backend_name] {
        return std::make_shared<LlmNodeBackend>(make_llm(), backend_name);
      });
  // 数据面事件出口：--events 指定时绑定发布端点并注入节点外壳，
  // 生成 token/done 实时发布（订阅者先行握手，节点侧不阻塞等待）。
  std::shared_ptr<slotnexus::dataplane::EventPublisher> event_pub;
  if (!events_endpoint.empty()) {
    event_pub = std::make_shared<slotnexus::dataplane::EventPublisher>(ctx);
    event_pub->bind(events_endpoint, events_sync);
  }
  slotnexus::node::RuntimeNode node(
      ctx, std::move(runtime),
      std::chrono::milliseconds(infer_timeout_ms), event_pub);
  try {
    node.bind(listen);
    std::cout << "llm_node 监听 " << listen << "（" << backend_name << " 后端";
    if (backend_name == "rkllm") {
      std::cout << "，模型 " << llm_options.model_path
                << "，max_new_tokens " << llm_options.max_new_tokens
                << " / max_context_len " << llm_options.max_context_len
                << "，top_k " << llm_options.top_k
                << " / top_p " << llm_options.top_p
                << " / temperature " << llm_options.temperature
                << "，enable_thinking "
                << (llm_options.enable_thinking ? 1 : 0)
                << "，reasoning_end_tag "
                << (llm_options.reasoning_end_tag.empty()
                        ? std::string("<off>")
                        : llm_options.reasoning_end_tag);
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
