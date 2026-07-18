// llm_node 可执行入口：Fake 大模型文本生成节点。
//
// 用法：llm_node [--listen tcp://127.0.0.1:19203]
// 默认端口约定：echo 19200 / asr 19201 / rag 19202 / llm 19203 / tts 19204。
//
// Node 外壳（RuntimeNode + TaskRuntime）只依赖接口；本文件实现 IBackend
// 适配器，把流式 FakeLlmBackend 驱动到完成并返回最终文本：
//   - Mock 负载约定：客户端发 {"text": "<prompt>"}；RuntimeNode 已提取
//     text 字段，适配器收到纯文本 prompt；
//   - 返回 payload.text = kDone 携带的完整输出文本（token 空白归一化回显）。
// Fake 用于测试协议与编排；未来 RkllmBackend 只换工厂。
// SIGINT/SIGTERM 优雅退出（退出码 0）。
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <zmq.hpp>

#include "voxorchestra/backend/backend_event.hpp"
#include "voxorchestra/backend/fake/fake_llm_backend.hpp"
#include "voxorchestra/runtime/ibackend.hpp"
#include "runtime_node.hpp"

namespace {

// IBackend 适配器：把 FakeLlmBackend 的流式事件驱动到完成。
class LlmNodeBackend final : public voxorchestra::runtime::IBackend {
 public:
  voxorchestra::runtime::BackendResult infer(
      const std::string& payload,
      std::chrono::steady_clock::time_point /*deadline*/,
      const std::atomic<bool>& cancelled) override {
    if (cancelled.load()) {
      llm_.cancel();
      return {voxorchestra::runtime::BackendResult::Code::kCancelled, {}};
    }
    std::string final_text;
    llm_.set_event_callback([&final_text](const voxorchestra::backend::BackendEvent& e) {
      if (e.kind == voxorchestra::backend::BackendEvent::Kind::kDone) {
        final_text = e.text;
      }
    });
    llm_.generate(payload);
    return {voxorchestra::runtime::BackendResult::Code::kOk, std::move(final_text)};
  }

 private:
  voxorchestra::backend::fake::FakeLlmBackend llm_;
};

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int /*sig*/) { g_stop = 1; }

}  // namespace

int main(int argc, char** argv) {
  std::string listen = "tcp://127.0.0.1:19203";
  for (int i = 1; i < argc - 1; ++i) {
    if (std::string(argv[i]) == "--listen") {
      listen = argv[i + 1];
    }
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  zmq::context_t ctx(1);
  auto runtime = std::make_unique<voxorchestra::runtime::TaskRuntime>(
      [] { return std::make_shared<LlmNodeBackend>(); });
  voxorchestra::node::RuntimeNode node(ctx, std::move(runtime));
  try {
    node.bind(listen);
    std::cout << "llm_node 监听 " << listen << "（FakeLlm 后端）" << std::endl;
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
