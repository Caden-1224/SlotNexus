// echo_node 可执行入口：Echo 模拟推理节点。
//
// 用法：echo_node [--listen tcp://127.0.0.1:19200]
// 使用默认后端工厂（EchoBackend：立即返回 "echo:" + 输入）。
// 未来 asr_node / llm_node / tts_node 复用 RuntimeNode，只换后端工厂。
// SIGINT/SIGTERM 优雅退出（退出码 0）。
#include <csignal>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <zmq.hpp>

#include "runtime_node.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int /*sig*/) { g_stop = 1; }

}  // namespace

int main(int argc, char** argv) {
  std::string listen = "tcp://127.0.0.1:19200";
  for (int i = 1; i < argc - 1; ++i) {
    if (std::string(argv[i]) == "--listen") {
      listen = argv[i + 1];
    }
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  zmq::context_t ctx(1);
  auto runtime = std::make_unique<voxorchestra::runtime::TaskRuntime>();  // Echo 工厂
  voxorchestra::node::RuntimeNode node(ctx, std::move(runtime));
  try {
    node.bind(listen);
    std::cout << "echo_node 监听 " << listen << "（Echo 后端）" << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "echo_node 启动失败: " << e.what() << std::endl;
    return 1;
  }

  while (!g_stop) {
    node.serve_once(std::chrono::milliseconds(100));
  }
  node.close();
  std::cout << "echo_node 已退出" << std::endl;
  return 0;
}
