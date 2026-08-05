// rag_node 可执行入口：真实 BM25 检索节点（JSONL 知识库 + L0-L3 分级路由）。
//
// 用法：rag_node [--listen tcp://127.0.0.1:19202] [--knowledge <jsonl>]
//                [--direct-threshold <v>] [--context-threshold <v>]
//                [--top-k N]
// 默认端口约定：echo 19200 / asr 19201 / rag 19202 / llm 19203 / tts 19204。
//
// Node 外壳（RuntimeNode + TaskRuntime）只依赖接口；本文件实现 IBackend
// 适配器，把查询送入真实 Router（KnowledgeStore + BM25 + L0-L3 路由）：
//   - 请求负载约定：客户端发 {"text": "<查询>"}；RuntimeNode 已提取 text
//     字段，适配器收到纯文本查询串；
//   - 返回 payload.text = JSON 对象：
//       {"level": "l0|l1|l2|l3", "top1_score": <v>,
//        "answer": "...", "prompt": "...",
//        "chunks": [{"id","text","score"}, ...]}   // 得分降序
//     L0/L1 有 answer（直答文本）、L2/L3 有 prompt（送入 LLM 的提示词），
//     另一字段为空串；chunks 在 L0/L3 为空数组，L1/L2 为命中块（含得分）。
// 路由算法与 session_node 内嵌 Router 同源（libs/rag），语义一致：
// 知识库加载失败视为配置错误，进程启动即失败退出。
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

#include "voxorchestra/rag/knowledge_store.hpp"
#include "voxorchestra/rag/router.hpp"
#include "voxorchestra/runtime/ibackend.hpp"
#include "runtime_node.hpp"

namespace {

// IBackend 适配器：查询 → Router.route() → 路由决策 JSON（含级别与证据）。
class RagNodeBackend final : public voxorchestra::runtime::IBackend {
 public:
  explicit RagNodeBackend(std::shared_ptr<const voxorchestra::rag::Router> router)
      : router_(std::move(router)) {}

  voxorchestra::runtime::BackendResult infer(
      const std::string& payload,
      std::chrono::steady_clock::time_point /*deadline*/,
      const std::atomic<bool>& cancelled,
      const voxorchestra::runtime::EventSink& /*events*/) override {
    if (cancelled.load()) {
      return {voxorchestra::runtime::BackendResult::Code::kCancelled, {}};
    }
    const auto d = router_->route(payload);
    nlohmann::json out;
    out["level"] = voxorchestra::rag::to_string(d.level);
    out["top1_score"] = d.top1_score;
    out["answer"] = d.answer;
    out["prompt"] = d.prompt;
    nlohmann::json chunks = nlohmann::json::array();
    for (const auto& c : d.chunks) {
      chunks.push_back({{"id", c.id}, {"text", c.text}, {"score", c.score}});
    }
    out["chunks"] = std::move(chunks);
    return {voxorchestra::runtime::BackendResult::Code::kOk, out.dump()};
  }

 private:
  std::shared_ptr<const voxorchestra::rag::Router> router_;
};

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int /*sig*/) { g_stop = 1; }

double parse_double(const char* s, double fallback) {
  try {
    return std::stod(s);
  } catch (...) {
    return fallback;
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string listen = "tcp://127.0.0.1:19202";
  std::string knowledge_path = "data/knowledge/knowledge.jsonl";
  voxorchestra::rag::RouterConfig router_cfg;  // 默认阈值；命令行可覆盖
  for (int i = 1; i < argc - 1; ++i) {
    const std::string arg = argv[i];
    const std::string val = argv[i + 1];
    if (arg == "--listen") {
      listen = val;
    } else if (arg == "--knowledge") {
      knowledge_path = val;
    } else if (arg == "--direct-threshold") {
      router_cfg.direct_threshold = parse_double(val.c_str(),
                                                 router_cfg.direct_threshold);
    } else if (arg == "--context-threshold") {
      router_cfg.context_threshold =
          parse_double(val.c_str(), router_cfg.context_threshold);
    } else if (arg == "--top-k") {
      try {
        router_cfg.top_k = static_cast<std::size_t>(std::stoi(val));
      } catch (...) {
        // 非法值保持默认
      }
    }
  }

  // 知识库 → BM25 索引 → L0-L3 路由（进程级只读，与 session_node 同源组装）。
  std::shared_ptr<const voxorchestra::rag::Router> router;
  try {
    const voxorchestra::rag::KnowledgeStore store(knowledge_path);
    voxorchestra::rag::Bm25Index index;
    for (const auto& e : store.entries()) {
      index.add_document(e.text);
    }
    index.build();
    router = std::make_shared<const voxorchestra::rag::Router>(
        std::move(index), store.entries(), router_cfg);
    std::cout << "rag_node 知识库 " << knowledge_path << "（" << store.size()
              << " 条）direct=" << router_cfg.direct_threshold
              << " context=" << router_cfg.context_threshold
              << " top-k=" << router_cfg.top_k << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "rag_node 知识库加载失败: " << e.what() << std::endl;
    return 1;
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  zmq::context_t ctx(1);
  auto runtime = std::make_unique<voxorchestra::runtime::TaskRuntime>(
      [router] { return std::make_shared<RagNodeBackend>(router); });
  voxorchestra::node::RuntimeNode node(ctx, std::move(runtime));
  try {
    node.bind(listen);
    std::cout << "rag_node 监听 " << listen << "（真实 BM25 L0-L3 路由）"
              << std::endl;
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
