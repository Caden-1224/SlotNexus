// Gateway 集成测试：外部 JSON → 信封校验、转发给 Manager、结构化错误。
//
// 进程内 fake Manager：RpcServer 回 ack 信封（回显关联字段），验证
// 网关把 action 转发出去并把响应送回原连接。
#include "action_helpers.hpp"
#include "edge_gateway.hpp"
#include "voxorchestra/network/event_loop.hpp"
#include "voxorchestra/protocol/message_envelope.hpp"
#include "voxorchestra/transport/rpc.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <zmq.hpp>

namespace eg = voxorchestra::gateway;
using namespace std::chrono_literals;

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

// 与 tcp_server_test 相同的测试客户端（一次 recv 多行不丢失）。
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

constexpr const char* kFakeManagerEndpoint = "tcp://127.0.0.1:19555";

// fake Manager：把收到的 action 回 ack 信封（回显关联字段），模拟
// 真实 Manager 的响应形态。有独立线程服务，保证与网关的 RPC 并发。
std::string fake_manager_handler(const std::string& request_json) {
  try {
    const voxorchestra::protocol::MessageEnvelope req =
        voxorchestra::protocol::MessageEnvelope::from_json(request_json);
    return voxorchestra::app::BuildAck(req, {{"status", "ok"}}).to_json();
  } catch (...) {
    return R"({"version":1,"type":"error","error":{"code":1,"message":"bad json"}})";
  }
}

// 进程内网关：专用线程跑事件循环；with_fake_manager 时起 fake Manager 线程。
struct GatewayFixture {
  zmq::context_t ctx{1};
  voxorchestra::transport::RpcServer fake_manager{ctx};
  std::atomic<bool> fake_stop{false};
  std::thread fake_thread;
  voxorchestra::network::EventLoop loop;
  eg::EdgeGateway gateway{&loop, "127.0.0.1", 0,
                          kFakeManagerEndpoint};  // 默认连 fake Manager
  std::atomic<bool> started{false};
  std::thread thread;

  GatewayFixture(bool with_fake_manager = true) {
    if (with_fake_manager) {
      fake_manager.bind(kFakeManagerEndpoint);
      fake_thread = std::thread([this] {
        while (!fake_stop.load()) {
          fake_manager.serve_once_timeout(fake_manager_handler, 50ms);
        }
      });
    }
    thread = std::thread([this] { loop.run(); });
    loop.run_in_loop([this] {
      gateway.start();
      started.store(true);
    });
  }

  std::uint16_t port() {
    while (!started.load()) {
      std::this_thread::sleep_for(1ms);
    }
    return gateway.local_port();
  }

  ~GatewayFixture() {
    fake_stop.store(true);
    if (fake_thread.joinable()) {
      fake_thread.join();  // serve_once 50ms 轮询，很快退出
    }
    gateway.stop();
    loop.quit();
    thread.join();
  }
};

void test_forwarded_request_gets_ack() {
  GatewayFixture g;
  TestClient c;
  CHECK(c.connect_to(g.port()));

  const std::string req =
      R"({"version":1,"type":"inference","work_id":"w-1","request_id":"r-1","session_id":"s-1"})" "\n";
  CHECK(c.send_all(req));

  std::string reply;
  CHECK(c.recv_line(reply, 2000ms));
  // 网关转发给 Manager，Manager 的 ack 信封经网关送回原连接。
  CHECK(reply.find(R"("type":"ack")") != std::string::npos);
  CHECK(reply.find(R"("request_id":"r-1")") != std::string::npos);
  CHECK(reply.find(R"("work_id":"w-1")") != std::string::npos);
  CHECK(reply.find(R"("session_id":"s-1")") != std::string::npos);
  std::cout << "  [ok] action 转发到 Manager，ack 信封送回原连接" << std::endl;
}

void test_invalid_json_gets_error_and_connection_survives() {
  GatewayFixture g;
  TestClient c;
  CHECK(c.connect_to(g.port()));

  CHECK(c.send_all("this is not json\n"));
  std::string reply;
  CHECK(c.recv_line(reply, 2000ms));
  CHECK(reply.find(R"("type":"error")") != std::string::npos);
  CHECK(reply.find("\"code\":1") != std::string::npos);  // kInvalidJson == 1

  // 错误后连接仍可用：再发一条合法 action，经 Manager 返回 ack。
  CHECK(c.send_all(R"({"version":1,"type":"setup","request_id":"r-2"})" "\n"));
  CHECK(c.recv_line(reply, 2000ms));
  CHECK(reply.find(R"("type":"ack")") != std::string::npos);
  CHECK(reply.find(R"("request_id":"r-2")") != std::string::npos);
  std::cout << "  [ok] 非法 JSON 返回结构化错误，连接保持可用" << std::endl;
}

void test_unknown_version_gets_error() {
  GatewayFixture g;
  TestClient c;
  CHECK(c.connect_to(g.port()));

  CHECK(c.send_all(R"({"version":99,"type":"event"})" "\n"));
  std::string reply;
  CHECK(c.recv_line(reply, 2000ms));
  CHECK(reply.find("\"code\":2") != std::string::npos);  // kUnknownVersion == 2
  std::cout << "  [ok] 未知版本返回错误码 2" << std::endl;
}

void test_unknown_type_gets_error() {
  GatewayFixture g;
  TestClient c;
  CHECK(c.connect_to(g.port()));

  CHECK(c.send_all(R"({"version":1,"type":"teleport"})" "\n"));
  std::string reply;
  CHECK(c.recv_line(reply, 2000ms));
  CHECK(reply.find("\"code\":3") != std::string::npos);  // kInvalidType == 3
  std::cout << "  [ok] 未知类型返回错误码 3" << std::endl;
}

void test_missing_type_gets_error() {
  GatewayFixture g;
  TestClient c;
  CHECK(c.connect_to(g.port()));

  CHECK(c.send_all(R"({"version":1,"work_id":"w-9"})" "\n"));
  std::string reply;
  CHECK(c.recv_line(reply, 2000ms));
  CHECK(reply.find("\"code\":4") != std::string::npos);  // kMissingField == 4
  std::cout << "  [ok] 缺少 type 返回错误码 4" << std::endl;
}

void test_client_direction_types_rejected() {
  GatewayFixture g;
  TestClient c;
  CHECK(c.connect_to(g.port()));

  // event/ack/error 是服务端→客户端方向，客户端发送被拒绝。
  CHECK(c.send_all(R"({"version":1,"type":"event","request_id":"r-3"})" "\n"));
  std::string reply;
  CHECK(c.recv_line(reply, 2000ms));
  CHECK(reply.find(R"("type":"error")") != std::string::npos);
  CHECK(reply.find("\"code\":3") != std::string::npos);
  CHECK(reply.find(R"("request_id":"r-3")") != std::string::npos);
  CHECK(reply.find("客户端不允许") != std::string::npos);
  std::cout << "  [ok] 客户端发送服务端方向类型（event）被拒绝" << std::endl;
}

void test_oversized_closes_connection() {
  GatewayFixture g;
  TestClient c;
  CHECK(c.connect_to(g.port()));

  std::string huge(1024 * 1024 + 8, 'x');
  CHECK(c.send_all(huge));

  std::string dummy;
  CHECK(!c.recv_line(dummy, 3000ms));  // 对端关闭，无回复
  std::cout << "  [ok] 超长帧直接断开连接（不回复）" << std::endl;
}

void test_manager_unreachable_gets_error() {
  // 不启 fake Manager，网关转发 3s 超时后回 manager_unreachable，连接保持。
  GatewayFixture g(false);
  TestClient c;
  CHECK(c.connect_to(g.port()));

  CHECK(c.send_all(R"({"version":1,"type":"inference","request_id":"r-4"})" "\n"));
  std::string reply;
  CHECK(c.recv_line(reply, 5000ms));
  CHECK(reply.find(R"("type":"error")") != std::string::npos);
  CHECK(reply.find("manager_unreachable") != std::string::npos);
  CHECK(reply.find(R"("request_id":"r-4")") != std::string::npos);

  // 错误后连接仍可用。
  CHECK(c.send_all("not json\n"));
  CHECK(c.recv_line(reply, 2000ms));
  CHECK(reply.find(R"("type":"error")") != std::string::npos);
  std::cout << "  [ok] Manager 不可达返回 manager_unreachable，连接保持可用" << std::endl;
}

}  // namespace

int main() {
  std::cout << "gateway_test:" << std::endl;
  test_forwarded_request_gets_ack();
  test_invalid_json_gets_error_and_connection_survives();
  test_unknown_version_gets_error();
  test_unknown_type_gets_error();
  test_missing_type_gets_error();
  test_client_direction_types_rejected();
  test_oversized_closes_connection();
  test_manager_unreachable_gets_error();

  if (g_failures == 0) {
    std::cout << "gateway_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "gateway_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
