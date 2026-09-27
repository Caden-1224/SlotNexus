// 通用数据面事件通道集成测试（inproc 端点）。
//
// Author: Caden
// 覆盖：握手防丢首条、kind + JSON payload 往返、index/finish 顺序、
// topic 精确过滤、接收超时与发布端关闭、非 event 信封协议校验。
#include "slotnexus/dataplane/dataplane_event.hpp"
#include "slotnexus/dataplane/event_channel.hpp"
#include "slotnexus/protocol/message_envelope.hpp"
#include "slotnexus/transport/pubsub.hpp"

#include <chrono>
#include <iostream>
#include <string>

#include <zmq.hpp>

namespace vd = slotnexus::dataplane;
namespace et = slotnexus::transport;
namespace pr = slotnexus::protocol;
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

void test_event_stream_order_and_finish() {
  zmq::context_t ctx(1);
  vd::EventSubscriber sub(ctx);
  sub.subscribe("w-1", "r-1");
  vd::EventPublisher pub(ctx);
  pub.bind("inproc://dp-order-data", "inproc://dp-order-sync");
  sub.connect("inproc://dp-order-data");
  sub.notify_ready("inproc://dp-order-sync");
  pub.wait_subscriber_ready(1000ms);

  vd::DataplaneEvent e;
  e.kind = "app.delta";
  for (int i = 0; i < 3; ++i) {
    e.payload = {{"seq", i}};
    e.index = i;
    pub.publish(e, "w-1", "r-1");
  }
  e.kind = "app.done";
  e.payload = {{"ok", true}};
  e.index = 3;
  e.finish = true;
  pub.publish(e, "w-1", "r-1");

  for (int i = 0; i < 3; ++i) {
    vd::DataplaneEvent got;
    CHECK(sub.recv(got, 1000ms));
    CHECK(got.kind == "app.delta");
    CHECK(got.payload.value("seq", -1) == i);
    CHECK(got.index == i);
    CHECK(!got.finish);
  }
  vd::DataplaneEvent got;
  CHECK(sub.recv(got, 1000ms));
  CHECK(got.kind == "app.done" && got.payload.value("ok", false));
  CHECK(got.index == 3 && got.finish);
  CHECK(!sub.recv(got, 200ms));
  std::cout << "  [ok] 通用事件按 index 顺序到达，finish 标记流尾" << std::endl;
}

void test_topic_isolation() {
  zmq::context_t ctx(1);
  vd::EventSubscriber sub_a(ctx);
  sub_a.subscribe("w-1", "r-1");
  vd::EventSubscriber sub_b(ctx);
  sub_b.subscribe("w-2", "r-1");
  vd::EventPublisher pub(ctx);
  pub.bind("inproc://dp-iso-data", "inproc://dp-iso-sync");
  sub_a.connect("inproc://dp-iso-data");
  sub_b.connect("inproc://dp-iso-data");
  sub_a.notify_ready("inproc://dp-iso-sync");
  sub_b.notify_ready("inproc://dp-iso-sync");
  pub.wait_subscriber_ready(1000ms);

  vd::DataplaneEvent e;
  e.kind = "app.token";
  e.index = 0;
  e.payload = {{"text", "a"}};
  pub.publish(e, "w-1", "r-1");
  e.payload = {{"text", "b"}};
  pub.publish(e, "w-2", "r-1");
  e.payload = {{"text", "c"}};
  e.index = 1;
  pub.publish(e, "w-1", "r-1");

  vd::DataplaneEvent got;
  CHECK(sub_a.recv(got, 1000ms));
  CHECK(got.payload.value("text", "") == "a");
  CHECK(sub_b.recv(got, 1000ms));
  CHECK(got.payload.value("text", "") == "b");
  CHECK(sub_a.recv(got, 1000ms));
  CHECK(got.payload.value("text", "") == "c");
  CHECK(!sub_a.recv(got, 200ms));
  CHECK(!sub_b.recv(got, 200ms));
  std::cout << "  [ok] 不同 work_id 的事件流互不串扰" << std::endl;
}

void test_topic_prefix_precision() {
  zmq::context_t ctx(1);
  vd::EventSubscriber sub(ctx);
  sub.subscribe("w-1", "r-2");
  vd::EventPublisher pub(ctx);
  pub.bind("inproc://dp-prec-data", "inproc://dp-prec-sync");
  sub.connect("inproc://dp-prec-data");
  sub.notify_ready("inproc://dp-prec-sync");
  pub.wait_subscriber_ready(1000ms);

  vd::DataplaneEvent e;
  e.kind = "app.pcm";
  e.index = 0;
  e.payload = {{"bytes", 320}};
  pub.publish(e, "w-1", "r-2");
  pub.publish(e, "w-1", "r-20");
  pub.publish(e, "w-1", "r-3");

  vd::DataplaneEvent got;
  CHECK(sub.recv(got, 1000ms));
  CHECK(!sub.recv(got, 200ms));
  std::cout << "  [ok] 尾斜杠主题约定精确过滤相近 request_id" << std::endl;
}

void test_recv_timeout_and_close() {
  zmq::context_t ctx(1);
  vd::EventSubscriber sub(ctx);
  sub.subscribe("w-1", "r-1");
  vd::EventPublisher pub(ctx);
  pub.bind("inproc://dp-timeout-data", "inproc://dp-timeout-sync");
  sub.connect("inproc://dp-timeout-data");
  sub.notify_ready("inproc://dp-timeout-sync");
  pub.wait_subscriber_ready(1000ms);

  vd::DataplaneEvent got;
  CHECK(!sub.recv(got, 200ms));
  pub.close();
  CHECK(!sub.recv(got, 500ms));
  std::cout << "  [ok] 空流超时与发布端关闭均快速返回 false" << std::endl;
}

void test_protocol_validation() {
  zmq::context_t ctx(1);
  et::PubSocket raw_pub(ctx);
  raw_pub.bind("inproc://dp-proto-data");
  raw_pub.bind_sync("inproc://dp-proto-sync");
  vd::EventSubscriber sub(ctx);
  sub.subscribe("w-1", "r-1");
  sub.connect("inproc://dp-proto-data");
  sub.notify_ready("inproc://dp-proto-sync");
  raw_pub.wait_subscriber_ready(1000ms);

  pr::MessageEnvelope bad;
  bad.set_type(pr::MessageType::kAck);
  bad.set_work_id("w-1");
  bad.set_request_id("r-1");
  raw_pub.publish("w-1/r-1/", bad.to_json());

  vd::DataplaneEvent got;
  bool threw = false;
  try {
    sub.recv(got, 1000ms);
  } catch (const pr::ProtocolError&) {
    threw = true;
  }
  CHECK(threw);
  std::cout << "  [ok] 非事件信封触发协议校验异常" << std::endl;
}

}  // namespace

int main() {
  std::cout << "dataplane_test:" << std::endl;
  test_event_stream_order_and_finish();
  test_topic_isolation();
  test_topic_prefix_precision();
  test_recv_timeout_and_close();
  test_protocol_validation();

  if (g_failures == 0) {
    std::cout << "dataplane_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "dataplane_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
