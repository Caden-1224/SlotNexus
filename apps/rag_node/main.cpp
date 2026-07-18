// rag_node 可执行入口：Fake 文本检索节点。
//
// 用法：rag_node [--listen tcp://127.0.0.1:19202] [--top-k N]（默认 2）
// 默认端口约定：echo 19200 / asr 19201 / rag 19202 / llm 19203 / tts 19204。
//
// Node 外壳（RuntimeNode + TaskRuntime）只依赖接口；本文件实现 IBackend
// 适配器，把查询送入 FakeRetriever 并序列化结果：
//   - Mock 负载约定：客户端发 {"text": "<查询>"}；RuntimeNode 已提取 text
//     字段，适配器收到纯文本查询串；
//   - 返回 payload.text = Top-K 块的 JSON 数组字符串（得分降序）：
//     [{"id": "...", "text": "...", "score": 0.95}, ...]
// Fake 用于测试协议与编排；未来 BM25/真实检索只换工厂。
// SIGINT/SIGTERM 优雅退出（退出码 0）。
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>
#include <zmq.hpp>

#include "voxorchestra/backend/fake/fake_retriever.hpp"
#include "voxorchestra/runtime/ibackend.hpp"
#include "runtime_node.hpp"

namespace {

// IBackend 适配器：查询 → FakeRetriever Top-K → JSON 数组字符串。
class RagNodeBackend final : public voxorchestra::runtime::IBackend {
 public:
  explicit RagNodeBackend(std::size_t top_k) : top_k_(top_k) {}

  voxorchestra::runtime::BackendResult infer(
      const std::string& payload,
      std::chrono::steady_clock::time_point /*deadline*/,
      const std::atomic<bool>& cancelled) override {
    if (cancelled.load()) {
      return {voxorchestra::runtime::BackendResult::Code::kCancelled, {}};
    }
    nlohmann::json chunks = nlohmann::json::array();
    for (const auto& c : retriever_.retrieve(payload, top_k_)) {
      chunks.push_back({{"id", c.id}, {"text", c.text}, {"score", c.score}});
    }
    return {voxorchestra::runtime::BackendResult::Code::kOk, chunks.dump()};
  }

 private:
  voxorchestra::backend::fake::FakeRetriever retriever_;
  std::size_t top_k_;
};

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int /*sig*/) { g_stop = 1; }

}  // namespace

int main(int argc, char** argv) {
  std::string listen = "tcp://127.0.0.1:19202";
  std::size_t top_k = 2;
  for (int i = 1; i < argc - 1; ++i) {
    if (std::string(argv[i]) == "--listen") {
      listen = argv[i + 1];
    } else if (std::string(argv[i]) == "--top-k") {
      try {
        top_k = static_cast<std::size_t>(std::stoi(argv[i + 1]));
      } catch (...) {
        top_k = 2;
      }
    }
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  zmq::context_t ctx(1);
  auto runtime = std::make_unique<voxorchestra::runtime::TaskRuntime>(
      [top_k] { return std::make_shared<RagNodeBackend>(top_k); });
  voxorchestra::node::RuntimeNode node(ctx, std::move(runtime));
  try {
    node.bind(listen);
    std::cout << "rag_node 监听 " << listen << "（FakeRetriever 后端，Top-K="
              << top_k << "）" << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "rag_node 启动失败: " << e.what() << std::endl;
    return 1;
  }

  while (!g_stop) {
    node.serve_once(std::chrono::milliseconds(100));
  }
  node.close();
  std::cout << "rag_node 已退出" << std::endl;
  return 0;
}
