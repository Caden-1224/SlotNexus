// RPC 集成测试：往返、超时与超时后重试、关闭、context 终止。
//
// 失败报告纪律：本文件里有两类"偶发即整进程崩溃"的路径，都已封堵——
//   1) 客户端 call() 未捕获异常：任何一次超过 deadline 的抖动都会让异常逃出
//      main，表现为 "Subprocess aborted"（SIGABRT），把一次偶发超时伪装成
//      崩溃并掩盖真实原因；统一走 call_expect_ok，降级为一条失败断言。
//   2) 服务线程未捕获 kClosed：close() 只置标志、不打断阻塞中的 recv，服务
//      循环可能在下一次入口才看到标志并抛 kClosed，异常逃出线程函数同样触发
//      std::terminate；服务循环统一走 serve_until_stopped，把 kClosed 当作
//      约定的优雅退出。
// 两处都只改变"失败如何被报告"，不改变被测语义与断言内容。
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

// 调用并断言成功：异常转成一条失败断言（返回空串由调用方 CHECK 兜住）。
std::string call_expect_ok(et::RpcClient& client, const std::string& request,
                           std::chrono::milliseconds deadline, int line) {
  try {
    return client.call(request, deadline);
  } catch (const std::exception& e) {
    ++g_failures;
    std::cerr << "FAIL " << __FILE__ << ":" << line
              << ": client.call 抛出异常: " << e.what() << std::endl;
    return std::string();
  }
}

// 服务循环：idle 内无请求，或收到 close()（下一次入口抛 kClosed）时正常退出。
void serve_until_stopped(et::RpcServer& server,
                         const et::RpcServer::Handler& handler,
                         std::chrono::milliseconds idle) {
  for (;;) {
    try {
      if (!server.serve_once_timeout(handler, idle)) {
        return;  // idle 内无新请求，避免死循环
      }
    } catch (const et::TransportError& e) {
      if (e.code() == et::TransportErrorCode::kClosed) {
        return;  // 约定的优雅退出
      }
      ++g_failures;
      std::cerr << "FAIL " << __FILE__ << ": 服务循环异常: " << e.what()
                << std::endl;
      return;
    } catch (const std::exception& e) {
      ++g_failures;
      std::cerr << "FAIL " << __FILE__ << ": 服务循环非 TransportError: "
                << e.what() << std::endl;
      return;
    }
  }
}

void test_round_trip() {
  zmq::context_t ctx(1);
  et::RpcServer server(ctx);
  server.bind("inproc://rpc-test");

  et::RpcClient client(ctx);
  client.connect("inproc://rpc-test");

  // 回显 "hello:xxx" 请求。
  const et::RpcServer::Handler echo = [](const std::string& req) {
    return "reply:" + req;
  };
  std::thread worker(
      [&server, &echo] { serve_until_stopped(server, echo, 200ms); });

  const std::string reply = call_expect_ok(client, "hello", 1000ms, __LINE__);
  CHECK(reply == "reply:hello");
  const std::string reply2 = call_expect_ok(client, "world", 1000ms, __LINE__);
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
  const et::RpcServer::Handler slow = [](const std::string& req) {
    std::this_thread::sleep_for(500ms);
    return "slow:" + req;
  };
  std::thread worker([&server, &slow] {
    serve_until_stopped(server, slow, 500ms);
    server.close();
  });

  et::RpcClient client(ctx);
  client.connect("inproc://rpc-timeout");

  // 第一次：deadline 100ms < 服务端 500ms 延迟 → 超时。
  CHECK_THROWS_CODE(client.call("x", 100ms), et::TransportErrorCode::kTimeout);

  // 超时后 REQ socket 已被内部重建，第二次直接重试应成功。
  const std::string reply = call_expect_ok(client, "y", 2000ms, __LINE__);
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
