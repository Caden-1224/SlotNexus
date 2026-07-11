// PUB/SUB 集成测试：订阅握手防丢首条、主题过滤、接收超时。
#include "voxorchestra/transport/pubsub.hpp"
#include "voxorchestra/transport/transport_error.hpp"

#include <chrono>
#include <iostream>
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

void test_sync_handshake_and_order() {
  zmq::context_t ctx(1);

  // 先创建订阅端并订阅，再建发布端，最后握手，保证首条消息不丢。
  et::SubSocket sub(ctx);
  sub.subscribe("asr");

  et::PubSocket pub(ctx);
  pub.bind("inproc://pub-data");
  pub.bind_sync("inproc://pub-sync");

  sub.connect("inproc://pub-data");
  sub.notify_ready("inproc://pub-sync");
  pub.wait_subscriber_ready(1000ms);

  // 握手完成前消息绝不会发布：发布 10 条并全部按序收到。
  for (int i = 0; i < 10; ++i) {
    pub.publish("asr", "frame-" + std::to_string(i));
  }
  for (int i = 0; i < 10; ++i) {
    std::string topic, payload;
    CHECK(sub.recv(topic, payload, 1000ms));
    CHECK(topic == "asr");
    CHECK(payload == "frame-" + std::to_string(i));
  }
  std::cout << "  [ok] 握手后 10 条消息按序全部收到（首条未丢）" << std::endl;
}

void test_topic_filter() {
  zmq::context_t ctx(1);

  et::SubSocket sub(ctx);
  sub.subscribe("asr");  // 只订阅 asr 前缀

  et::PubSocket pub(ctx);
  pub.bind("inproc://pub-filter");
  pub.bind_sync("inproc://pub-filter-sync");

  sub.connect("inproc://pub-filter");
  sub.notify_ready("inproc://pub-filter-sync");
  pub.wait_subscriber_ready(1000ms);

  pub.publish("asr", "a1");
  pub.publish("tts", "should-be-dropped");
  pub.publish("asr", "a2");

  std::string topic, payload;
  CHECK(sub.recv(topic, payload, 1000ms));
  CHECK(topic == "asr" && payload == "a1");
  CHECK(sub.recv(topic, payload, 1000ms));
  CHECK(topic == "asr" && payload == "a2");

  // tts 主题被过滤，不应收到第三条；超时返回 false。
  CHECK(!sub.recv(topic, payload, 200ms));
  std::cout << "  [ok] 主题过滤生效，未订阅主题被丢弃" << std::endl;
}

void test_recv_timeout() {
  zmq::context_t ctx(1);

  et::SubSocket sub(ctx);
  sub.subscribe("");
  et::PubSocket pub(ctx);
  pub.bind("inproc://pub-empty");
  pub.bind_sync("inproc://pub-empty-sync");
  sub.connect("inproc://pub-empty");
  sub.notify_ready("inproc://pub-empty-sync");
  pub.wait_subscriber_ready(1000ms);

  std::string topic, payload;
  CHECK(!sub.recv(topic, payload, 200ms));  // 无消息 → 超时返回 false

  pub.close();  // 发布端先关，订阅端 recv 应快速返回 false
  CHECK(!sub.recv(topic, payload, 500ms));
  std::cout << "  [ok] 空队列接收超时返回 false，发布端关闭后不挂起" << std::endl;
}

}  // namespace

int main() {
  std::cout << "pubsub_integration_test:" << std::endl;
  test_sync_handshake_and_order();
  test_topic_filter();
  test_recv_timeout();

  if (g_failures == 0) {
    std::cout << "pubsub_integration_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "pubsub_integration_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
