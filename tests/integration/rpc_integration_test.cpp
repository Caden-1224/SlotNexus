// RPC 集成测试：往返、超时与超时后重试、关闭、context 终止。
#include "voxorchestra/transport/rpc.hpp"
#include "voxorchestra/transport/transport_error.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <zmq.hpp>

namespace et = voxorchestra::transport;
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

#define CHECK_THROWS_CODE(expr, expected_code)                               \
  do {                                                                       \
    bool caught = false;                                                     \
    try {                                                                    \
      expr;                                                                  \
    } catch (const et::TransportError& e) {                                  \
      caught = true;                                                         \
      if (e.code() != (expected_code)) {                                     \
        ++g_failures;                                                        \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__                  \
                  << ": 错误码不符，期望 " << static_cast<int>(expected_code) \
                  << " 实际 " << static_cast<int>(e.code()) << std::endl;    \
      }                                                                      \
    } catch (const std::exception& e) {                                      \
      ++g_failures;                                                          \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__                    \
                  << ": 抛出非 TransportError: " << e.what() << std::endl;   \
    }                                                                        \
    if (!caught) {                                                           \
      ++g_failures;                                                          \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__                    \
                  << ": 未抛出异常: " << #expr << std::endl;                  \
    }                                                                        \
  } while (0)

void test_round_trip() {
  zmq::context_t ctx(1);
  et::RpcServer server(ctx);
  server.bind("inproc://rpc-test");

  et::RpcClient client(ctx);
  client.connect("inproc://rpc-test");

  std::thread worker([&server] {
    // 回显 "hello:xxx" 请求。
    while (true) {
      const bool served = server.serve_once_timeout(
          [](const std::string& req) { return "reply:" + req; }, 200ms);
      if (!served) {
        break;  // 无新请求即退出，避免死循环
      }
    }
  });

  const std::string reply = client.call("hello", 1000ms);
  CHECK(reply == "reply:hello");
  const std::string reply2 = client.call("world", 1000ms);
  CHECK(reply2 == "reply:world");

  server.close();  // 让 worker 循环退出
  worker.join();
  std::cout << "  [ok] 往返一致（连续两次调用）" << std::endl;
}

void test_timeout_and_retry() {
  zmq::context_t ctx(1);
  et::RpcServer server(ctx);
  server.bind("inproc://rpc-timeout");

  // 服务端每条请求先睡 500ms 再应答；无新请求 500ms 后自动退出。
  std::thread worker([&server] {
    while (true) {
      const bool served = server.serve_once_timeout(
          [](const std::string& req) {
            std::this_thread::sleep_for(500ms);
            return "slow:" + req;
          },
          500ms);
      if (!served) {
        break;
      }
    }
    server.close();
  });

  et::RpcClient client(ctx);
  client.connect("inproc://rpc-timeout");

  // 第一次：deadline 100ms < 服务端 500ms 延迟 → 超时。
  CHECK_THROWS_CODE(client.call("x", 100ms), et::TransportErrorCode::kTimeout);

  // 超时后 REQ socket 已被内部重建，第二次直接重试应成功。
  const std::string reply = client.call("y", 2000ms);
  CHECK(reply == "slow:y");

  worker.join();
  std::cout << "  [ok] 超时抛错，重建后重试成功" << std::endl;
}

void test_close_behavior() {
  zmq::context_t ctx(1);
  et::RpcClient client(ctx);
  client.connect("inproc://never-bound");
  client.close();
  // 幂等：重复 close 不抛。
  client.close();
  CHECK_THROWS_CODE(client.call("x", 100ms), et::TransportErrorCode::kClosed);
  std::cout << "  [ok] close 幂等，关闭后调用抛 kClosed" << std::endl;
}

// 注：不做"先销毁 context 再操作 socket"的测试。zmq_ctx_term 会阻塞等待
// 所有 socket 关闭，该模式必然死锁；错误路径只验证 close() 语义。

}  // namespace

int main() {
  std::cout << "rpc_integration_test:" << std::endl;
  test_round_trip();
  test_timeout_and_retry();
  test_close_behavior();

  if (g_failures == 0) {
    std::cout << "rpc_integration_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "rpc_integration_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
