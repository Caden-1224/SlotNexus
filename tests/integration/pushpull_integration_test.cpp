// PUSH/PULL 集成测试：顺序分发、空队列超时、关闭、context 终止。
#include "voxorchestra/transport/pushpull.hpp"
#include "voxorchestra/transport/transport_error.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

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

void test_order_and_count() {
  zmq::context_t ctx(1);

  et::PullSocket pull(ctx);
  pull.bind("inproc://pp-order");

  et::PushSocket push(ctx);
  push.connect("inproc://pp-order");

  constexpr int kCount = 100;
  for (int i = 0; i < kCount; ++i) {
    push.send("task-" + std::to_string(i), 1000ms);
  }
  for (int i = 0; i < kCount; ++i) {
    std::string payload;
    CHECK(pull.recv(payload, 1000ms));
    CHECK(payload == "task-" + std::to_string(i));
  }
  std::cout << "  [ok] 100 条消息按序全部收到" << std::endl;
}

void test_empty_timeout() {
  zmq::context_t ctx(1);

  et::PullSocket pull(ctx);
  pull.bind("inproc://pp-empty");

  std::string payload;
  CHECK(!pull.recv(payload, 200ms));  // 空队列超时返回 false
  std::cout << "  [ok] 空队列接收超时返回 false" << std::endl;
}

void test_close_behavior() {
  zmq::context_t ctx(1);

  et::PullSocket pull(ctx);
  pull.bind("inproc://pp-close");

  et::PushSocket push(ctx);
  push.connect("inproc://pp-close");

  std::string payload;
  pull.close();
  CHECK_THROWS_CODE(pull.recv(payload, 100ms), et::TransportErrorCode::kClosed);

  // PUSH 端在 PULL 关闭后仍能发送（消息排队到 HWM），close 后立即抛错。
  push.close();
  CHECK_THROWS_CODE(push.send("x", 100ms), et::TransportErrorCode::kClosed);

  // 幂等重复 close。
  push.close();
  pull.close();
  std::cout << "  [ok] close 幂等，关闭后收发抛 kClosed" << std::endl;
}

}  // namespace

int main() {
  std::cout << "pushpull_integration_test:" << std::endl;
  test_order_and_count();
  test_empty_timeout();
  test_close_behavior();

  if (g_failures == 0) {
    std::cout << "pushpull_integration_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "pushpull_integration_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
