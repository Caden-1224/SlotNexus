// edge_gateway 可执行入口。
//
// 用法：edge_gateway [--port 9100]
// SIGINT/SIGTERM 优雅退出。
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "edge_gateway.hpp"
#include "voxorchestra/network/event_loop.hpp"

namespace {

voxorchestra::network::EventLoop* g_loop = nullptr;

void handle_signal(int /*sig*/) {
  // 只做原子置位 + eventfd 唤醒（write 是异步信号安全调用），
  // 不在信号上下文做复杂操作。
  if (g_loop != nullptr) {
    g_loop->quit();
  }
}

std::uint16_t parse_port(int argc, char** argv) {
  std::uint16_t port = 9100;
  for (int i = 1; i < argc - 1; ++i) {
    if (std::string(argv[i]) == "--port") {
      port = static_cast<std::uint16_t>(std::atoi(argv[i + 1]));
    }
  }
  return port;
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint16_t port = parse_port(argc, argv);

  voxorchestra::network::EventLoop loop;
  g_loop = &loop;

  voxorchestra::gateway::EdgeGateway gateway(&loop, "127.0.0.1", port);

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  loop.run_in_loop([&gateway] {
    try {
      gateway.start();
      std::cout << "edge_gateway 监听 127.0.0.1:" << gateway.local_port() << std::endl;
    } catch (const std::exception& e) {
      std::cerr << "网关启动失败: " << e.what() << std::endl;
      g_loop->quit();
    }
  });

  loop.run();
  std::cout << "edge_gateway 已退出" << std::endl;

  gateway.stop();
  g_loop = nullptr;
  return 0;
}
