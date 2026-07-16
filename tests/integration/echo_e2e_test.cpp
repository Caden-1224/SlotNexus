// Echo 端到端集成测试（端到端验收）：
//
// 真实进程拓扑：echo_node --ZMQ--> unit_manager <--ZMQ-- edge_gateway <--TCP-- 客户端
//   echo_node     监听 tcp://127.0.0.1:19200（Echo 后端）
//   unit_manager  监听 tcp://127.0.0.1:19100，路由到 echo_node
//   edge_gateway  监听 127.0.0.1:9100，转发 action 给 Manager
//
// 验收标准：20 轮 Echo E2E 无跨流；未知任务/取消/重复 exit 语义正确；
// SIGTERM 后三个进程全部优雅退出（退出码 0）。
#include "voxorchestra/protocol/message_envelope.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace ep = voxorchestra::protocol;
using ep::MessageEnvelope;
using ep::MessageType;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #cond   \
                << std::endl;                                                \
    }                                                                        \
  } while (0)

bool wait_until(const std::function<bool()>& cond, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) {
      return true;
    }
    std::this_thread::sleep_for(2ms);
  }
  return cond();
}

// 与 gateway_test 相同的测试客户端（一次 recv 多行不丢失）。
class TestClient {
 public:
  int fd = -1;
  std::string rx_buffer_;

  bool connect_to(std::uint16_t port) {
    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      return false;
    }
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    return ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0;
  }

  bool send_all(const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
      const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
      if (n <= 0) {
        return false;
      }
      sent += static_cast<std::size_t>(n);
    }
    return true;
  }

  bool recv_line(std::string& line, std::chrono::milliseconds timeout) {
    line.clear();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      const std::size_t nl = rx_buffer_.find('\n');
      if (nl != std::string::npos) {
        line = rx_buffer_.substr(0, nl);
        rx_buffer_.erase(0, nl + 1);
        return true;
      }
      struct pollfd pfd {.fd = fd, .events = POLLIN, .revents = 0};
      const int r = ::poll(&pfd, 1, 200);
      if (r < 0) {
        continue;
      }
      if (r == 0) {
        continue;
      }
      char buf[4096];
      const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n == 0) {
        return false;
      }
      if (n < 0) {
        return false;
      }
      rx_buffer_.append(buf, static_cast<std::size_t>(n));
    }
    return false;
  }

  ~TestClient() {
    if (fd >= 0) {
      ::close(fd);
    }
  }
};

// 子进程句柄：spawn / SIGTERM / 带超时等待退出码。
struct ChildProc {
  pid_t pid = -1;

  bool spawn(const std::string& exe, const std::vector<std::string>& args) {
    pid = fork();
    if (pid < 0) {
      return false;
    }
    if (pid == 0) {
      std::vector<char*> argv;
      argv.reserve(args.size() + 1);
      for (const auto& a : args) {
        argv.push_back(const_cast<char*>(a.c_str()));
      }
      argv.push_back(nullptr);
      ::execv(exe.c_str(), argv.data());
      ::_exit(127);  // execv 失败
    }
    return true;
  }

  void kill(int sig = SIGTERM) {
    if (pid > 0) {
      ::kill(pid, sig);
    }
  }

  // 等待进程退出；成功返回 true，exit_code 回填（非正常退出为 -1）。
  bool wait_for(std::chrono::milliseconds timeout, int* exit_code = nullptr) {
    if (pid <= 0) {
      return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      const pid_t r = ::waitpid(pid, &status, WNOHANG);
      if (r == pid) {
        if (exit_code != nullptr) {
          *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        return true;
      }
      std::this_thread::sleep_for(10ms);
    }
    return false;
  }
};

MessageEnvelope MakeRequest(MessageType type, const std::string& work_id,
                            const std::string& request_id,
                            const std::string& session_id = "",
                            nlohmann::json payload = nlohmann::json::object()) {
  MessageEnvelope e;
  e.set_type(type);
  e.set_work_id(work_id);
  e.set_request_id(request_id);
  e.set_session_id(session_id);
  e.set_payload(std::move(payload));
  return e;
}

// 发一条请求并解析响应；失败返回 false。
bool exchange(TestClient& c, const MessageEnvelope& req, MessageEnvelope& reply,
              std::chrono::milliseconds timeout) {
  if (!c.send_all(req.to_json() + "\n")) {
    return false;
  }
  std::string line;
  if (!c.recv_line(line, timeout)) {
    return false;
  }
  try {
    reply = MessageEnvelope::from_json(line);
    return true;
  } catch (...) {
    return false;
  }
}

constexpr std::uint16_t kGatewayPort = 9100;
constexpr const char* kEchoNodeExe = "../../apps/echo_node/echo_node";
constexpr const char* kUnitManagerExe = "../../apps/unit_manager/unit_manager";
constexpr const char* kEdgeGatewayExe = "../../apps/edge_gateway/edge_gateway";

void test_echo_e2e(const char* argv0) {
  // 测试二进制位于 build-wsl/tests/integration/，
  // apps 二进制位于其 ../../apps/<app>/<app>。
  std::string base = argv0;
  const auto slash = base.find_last_of('/');
  base = (slash == std::string::npos) ? "." : base.substr(0, slash);

  // 1. 拉起三个进程（顺序：节点 → Manager → 网关）。
  ChildProc node, manager, gateway;
  CHECK(node.spawn(base + "/" + kEchoNodeExe, {"echo_node"}));
  CHECK(manager.spawn(base + "/" + kUnitManagerExe, {"unit_manager"}));
  CHECK(gateway.spawn(base + "/" + kEdgeGatewayExe, {"edge_gateway"}));

  // 2. 等网关就绪（最多 5s），两个客户端都连上。
  TestClient a;
  TestClient b;
  CHECK(wait_until([&a] { return a.connect_to(kGatewayPort); }, 5000ms));
  CHECK(b.connect_to(kGatewayPort));

  // 3. 两个客户端各自 setup → Manager 分配全局 work_id。
  MessageEnvelope reply;
  CHECK(exchange(a, MakeRequest(MessageType::kSetup, "", "s-a", "sess-a"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kAck);
  CHECK(reply.work_id() == "w-0");
  CHECK(reply.session_id() == "sess-a");
  CHECK(exchange(b, MakeRequest(MessageType::kSetup, "", "s-b", "sess-b"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kAck);
  CHECK(reply.work_id() == "w-1");
  std::cout << "  [ok] 双客户端 setup：Manager 分配全局 work_id（w-0 / w-1）" << std::endl;

  // 4. 20 轮 Echo 推理：每 3 轮切到客户端 B，验证响应与 request_id
  //    一一对应（无跨流：不串任务、不串连接）。
  int rounds_a = 0;
  int rounds_b = 0;
  for (int i = 0; i < 20; ++i) {
    TestClient& c = (i % 3 == 0) ? b : a;
    const std::string work_id = (i % 3 == 0) ? "w-1" : "w-0";
    const std::string request_id = (i % 3 == 0) ? "rb-" + std::to_string(i)
                                                : "ra-" + std::to_string(i);
    const std::string text = "hello-" + std::to_string(i);
    CHECK(exchange(c, MakeRequest(MessageType::kInference, work_id, request_id,
                                  "sess-1", {{"text", text}}),
                   reply, 3000ms));
    CHECK(reply.type() == MessageType::kAck);
    CHECK(reply.request_id() == request_id);  // 无跨流的关键断言
    CHECK(reply.payload().value("text", std::string()) == "echo:" + text);
    if (i % 3 == 0) {
      ++rounds_b;
    } else {
      ++rounds_a;
    }
  }
  CHECK(rounds_a == 13 && rounds_b == 7);
  std::cout << "  [ok] 20 轮 Echo E2E 双任务交错推理：无跨流（13 轮 w-0 / 7 轮 w-1）"
            << std::endl;

  // 5. 未知任务：全链路返回 not_exist（Manager 路由未命中）。
  CHECK(exchange(a, MakeRequest(MessageType::kInference, "w-999", "rx-1",
                                "sess-1", {{"text", "x"}}),
                 reply, 3000ms));
  CHECK(reply.type() == MessageType::kError);
  CHECK(reply.error().code == 1);  // kNotExist
  CHECK(reply.error().message.find("未知任务") != std::string::npos);
  std::cout << "  [ok] 未知任务经全链路返回 not_exist" << std::endl;

  // 6. taskinfo：w-0 状态 ready，推理计数与上面 13 轮一致。
  CHECK(exchange(a, MakeRequest(MessageType::kTaskInfo, "w-0", "ti-1"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kAck);
  CHECK(reply.payload().value("state", std::string()) == "ready");
  CHECK(reply.payload().value("inference_count", 0) == 13);
  std::cout << "  [ok] taskinfo：state=ready，inference_count=13" << std::endl;

  // 7. cancel 空闲任务 / exit / 重复 exit。
  CHECK(exchange(a, MakeRequest(MessageType::kCancel, "w-0", "c-1"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kAck);
  CHECK(exchange(a, MakeRequest(MessageType::kExit, "w-0", "e-1"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kAck);
  CHECK(exchange(a, MakeRequest(MessageType::kExit, "w-0", "e-2"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kError);
  CHECK(reply.error().code == 1);  // 重复 exit → not_exist
  CHECK(exchange(b, MakeRequest(MessageType::kExit, "w-1", "e-3"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kAck);
  std::cout << "  [ok] cancel / exit / 重复 exit 语义正确（Manager 同步释放名额）"
            << std::endl;

  // 8. SIGTERM：三个进程全部优雅退出，退出码 0。
  gateway.kill();
  manager.kill();
  node.kill();
  int code = -1;
  CHECK(gateway.wait_for(5000ms, &code));
  CHECK(code == 0);
  CHECK(manager.wait_for(5000ms, &code));
  CHECK(code == 0);
  CHECK(node.wait_for(5000ms, &code));
  CHECK(code == 0);
  std::cout << "  [ok] SIGTERM 后三个进程全部优雅退出（退出码 0）" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
  std::cout << "echo_e2e_test:" << std::endl;
  test_echo_e2e(argc > 0 ? argv[0] : "echo_e2e_test");

  if (g_failures == 0) {
    std::cout << "echo_e2e_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "echo_e2e_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
