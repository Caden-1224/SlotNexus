// TcpServer 集成测试：用真实 socket 客户端验证
// 半包/粘包/回显、超长帧断开、提前断开、慢客户端写缓冲上限、优雅停止。
#include "voxorchestra/network/event_loop.hpp"
#include "voxorchestra/network/tcp_server.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace en = voxorchestra::network;
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

// 测试用 TCP 客户端封装。
class TestClient {
 public:
  int fd = -1;
  std::string rx_buffer_;  // 一次 recv 可能含多行，未消费的字节保留

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
      const ssize_t n =
          ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
      if (n <= 0) {
        return false;
      }
      sent += static_cast<std::size_t>(n);
    }
    return true;
  }

  // 读取一行（以 \n 结尾）；返回 false 表示超时或对端关闭。
  // 内部保留未消费的字节，一次 recv 带回多行时不会丢失。
  bool recv_line(std::string& line, std::chrono::milliseconds timeout) {
    line.clear();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      // 先消费缓冲里已有的完整行。
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
        continue;  // 超时继续等
      }
      char buf[4096];
      const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n == 0) {
        return false;  // 对端关闭
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

// 一个跑在专用线程的事件循环 + 回显服务器。
struct EchoServer {
  en::EventLoop loop;
  en::TcpServer server{&loop, "127.0.0.1", 0};
  std::atomic<bool> started{false};
  std::atomic<int> accept_count{0};
  std::atomic<int> close_count{0};
  std::string last_protocol_error;
  std::thread thread;

  explicit EchoServer(bool echo_reply = true) {
    server.set_connection_callback([this](const auto&) { accept_count.fetch_add(1); });
    server.set_close_callback([this](const auto&) { close_count.fetch_add(1); });
    server.set_protocol_error_callback(
        [this](const auto&, const std::string& m) { last_protocol_error = m; });
    if (echo_reply) {
      server.set_message_callback(
          [](const auto& conn, const std::string& frame) { conn->send("echo:" + frame + "\n"); });
    }
    thread = std::thread([this] { loop.run(); });
    loop.run_in_loop([this] {
      server.start();
      started.store(true);
    });
  }

  std::uint16_t port() {
    while (!started.load()) {
      std::this_thread::sleep_for(1ms);
    }
    return server.local_port();
  }

  ~EchoServer() {
    server.stop();
    loop.quit();
    thread.join();
  }
};

void test_echo_round_trip() {
  EchoServer s;
  TestClient c;
  CHECK(c.connect_to(s.port()));
  CHECK(c.send_all(R"({"type":"event","index":0})" "\n"));

  std::string reply;
  CHECK(c.recv_line(reply, 2000ms));
  CHECK(reply == R"(echo:{"type":"event","index":0})");
  std::cout << "  [ok] 回显往返一致" << std::endl;
}

void test_half_packet() {
  EchoServer s;
  TestClient c;
  CHECK(c.connect_to(s.port()));

  // 半包：一帧拆两次发送，中间有间隔。
  const std::string frame = R"({"type":"event","index":7})" "\n";
  const std::size_t split = frame.size() / 2;
  CHECK(c.send_all(frame.substr(0, split)));
  std::this_thread::sleep_for(50ms);
  CHECK(c.send_all(frame.substr(split)));

  std::string reply;
  CHECK(c.recv_line(reply, 2000ms));
  CHECK(reply == R"(echo:{"type":"event","index":7})");
  std::cout << "  [ok] 半包（两次 send）还原为完整帧" << std::endl;
}

void test_merged_frames() {
  EchoServer s;
  TestClient c;
  CHECK(c.connect_to(s.port()));

  // 粘包：一次 send 三个帧。
  const std::string data =
      R"({"type":"a"})" "\n" R"({"type":"b"})" "\n" R"({"type":"c"})" "\n";
  CHECK(c.send_all(data));

  for (int i = 0; i < 3; ++i) {
    std::string reply;
    CHECK(c.recv_line(reply, 2000ms));
    const std::string type = std::string(1, static_cast<char>('a' + i));
    CHECK(reply == std::string("echo:") + R"({"type":")" + type + R"("})");
  }
  std::cout << "  [ok] 粘包一次 send 多帧，按序逐个回显" << std::endl;
}

void test_oversized_frame_closes() {
  // 独立服务器：单帧超过 1 MiB 上限应触发协议错误并断开。
  en::EventLoop loop;
  en::TcpServer small(&loop, "127.0.0.1", 0);
  small.set_max_output_bytes(1 << 20);
  small.set_message_callback(
      [](const auto& conn, const std::string& f) { conn->send("echo:" + f + "\n"); });
  std::atomic<bool> proto_err{false};
  std::string err_msg;
  small.set_protocol_error_callback(
      [&](const auto&, const std::string& m) { proto_err.store(true); err_msg = m; });
  std::atomic<bool> started{false};
  std::thread t([&] {
    loop.run_in_loop([&] {
      small.start();
      started.store(true);
    });
    loop.run();
  });
  while (!started.load()) {
    std::this_thread::sleep_for(1ms);
  }

  TestClient c;
  CHECK(c.connect_to(small.local_port()));
  // 超过 1 MiB 且无换行 → 服务器应视为协议错误并关闭连接。
  std::string huge(1024 * 1024 + 8, 'x');
  CHECK(c.send_all(huge));

  std::string dummy;
  CHECK(!c.recv_line(dummy, 3000ms));  // 对端关闭 → 返回 false
  CHECK(wait_until([&] { return proto_err.load(); }, 2000ms));

  small.stop();
  loop.quit();
  t.join();
  std::cout << "  [ok] 超长帧触发协议错误并断开连接" << std::endl;
}

void test_early_disconnect_and_reconnect() {
  EchoServer s;
  {
    TestClient c;
    CHECK(c.connect_to(s.port()));
    CHECK(c.send_all(R"({"type":"a"})" "\n"));
    // 不等回复直接断开。
  }
  CHECK(wait_until([&] { return s.close_count.load() >= 1; }, 2000ms));

  // 服务器仍能接受新连接。
  TestClient c2;
  CHECK(c2.connect_to(s.port()));
  CHECK(c2.send_all(R"({"type":"b"})" "\n"));
  std::string reply;
  CHECK(c2.recv_line(reply, 2000ms));
  CHECK(reply == R"(echo:{"type":"b"})");
  std::cout << "  [ok] 提前断开被清理，新连接正常工作" << std::endl;
}

void test_slow_client_write_limit() {
  // 服务器回大量数据，客户端不读 → 写缓冲超限，服务器主动断开。
  en::EventLoop loop;
  en::TcpServer big(&loop, "127.0.0.1", 0);
  big.set_max_output_bytes(16 * 1024);
  big.set_message_callback([](const auto& conn, const std::string&) {
    conn->send(std::string(64 * 1024, 'Y'));  // 64 KiB >> 16 KiB 上限
  });
  std::atomic<bool> started{false};
  std::thread t([&] {
    loop.run_in_loop([&] {
      big.start();
      started.store(true);
    });
    loop.run();
  });
  while (!started.load()) {
    std::this_thread::sleep_for(1ms);
  }

  TestClient c;
  CHECK(c.connect_to(big.local_port()));
  CHECK(c.send_all("trigger\n"));  // 触发大回复

  // 客户端不读；服务器写缓冲超限后应关闭连接。
  std::string dummy;
  CHECK(!c.recv_line(dummy, 3000ms));  // 对端关闭
  std::cout << "  [ok] 慢客户端触发写缓冲上限，服务器断开连接" << std::endl;

  big.stop();
  loop.quit();
  t.join();
}

void test_disconnect_releases_connection() {
  // 回归：连接断开后 TcpServer 连接表必须清空（此前 close 回调按
  // c->fd() 擦除，而 fd 已被置 -1，erase(-1) 恒为 no-op，连接对象泄漏）。
  EchoServer s;
  {
    TestClient c1;
    TestClient c2;
    TestClient c3;
    CHECK(c1.connect_to(s.port()));
    CHECK(c2.connect_to(s.port()));
    CHECK(c3.connect_to(s.port()));
  }  // 三个客户端全部断开

  std::atomic<int> remaining{-1};
  CHECK(wait_until(
      [&] {
        s.loop.run_in_loop([&] { remaining.store(s.server.connection_count()); });
        return remaining.load() == 0;
      },
      2000ms));
  std::cout << "  [ok] 连接断开后连接表清空（无连接对象泄漏）" << std::endl;
}

void test_double_stop() {
  EchoServer s;
  s.server.stop();  // 重复 stop 幂等，不崩溃
  std::this_thread::sleep_for(50ms);
  std::cout << "  [ok] 重复 stop 幂等" << std::endl;
}

}  // namespace

int main() {
  std::cout << "tcp_server_test:" << std::endl;
  test_echo_round_trip();
  test_half_packet();
  test_merged_frames();
  test_oversized_frame_closes();
  test_early_disconnect_and_reconnect();
  test_slow_client_write_limit();
  test_disconnect_releases_connection();
  test_double_stop();

  if (g_failures == 0) {
    std::cout << "tcp_server_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "tcp_server_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
