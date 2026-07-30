// unit_manager 可执行入口。
//
// 用法：unit_manager [--listen tcp://127.0.0.1:19100] [--node tcp://127.0.0.1:19200]...
//   [--node-rpc-timeout-ms <ms>]（默认 3000；硬件后端模型加载可能数秒）
//   --node 可重复，多个节点时 setup 轮转分配。
// SIGINT/SIGTERM 优雅退出（退出码 0）。
#include <csignal>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <zmq.hpp>

#include "unit_manager.hpp"

namespace {

// 信号处理只做原子置位（sig_atomic_t 保证信号安全）。
volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int /*sig*/) { g_stop = 1; }

}  // namespace

int main(int argc, char** argv) {
  std::string listen = "tcp://127.0.0.1:19100";
  std::vector<std::string> nodes;
  long node_rpc_timeout_ms = 3000;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--listen" && i + 1 < argc) {
      listen = argv[++i];
    } else if (std::string(argv[i]) == "--node" && i + 1 < argc) {
      nodes.push_back(argv[++i]);
    } else if (std::string(argv[i]) == "--node-rpc-timeout-ms" && i + 1 < argc) {
      node_rpc_timeout_ms = std::atol(argv[++i]);
    }
  }
  if (nodes.empty()) {
    nodes.push_back("tcp://127.0.0.1:19200");
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  zmq::context_t ctx(1);
  voxorchestra::manager::UnitManager manager(
      ctx, nodes, /*max_tasks=*/0,
      std::chrono::milliseconds(node_rpc_timeout_ms));
  try {
    manager.bind(listen);
    std::cout << "unit_manager 监听 " << listen << "，节点 "
              << nodes.size() << " 个（RPC 超时 " << node_rpc_timeout_ms
              << " ms）" << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "unit_manager 启动失败: " << e.what() << std::endl;
    return 1;
  }

  // 服务循环：serve_once 每轮至多阻塞 100ms，信号到达后最迟 100ms 退出。
  while (!g_stop) {
    manager.serve_once(std::chrono::milliseconds(100));
  }
  manager.close();
  std::cout << "unit_manager 已退出" << std::endl;
  return 0;
}
