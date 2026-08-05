// 数据面事件通道集成测试（inproc 端点）：握手防丢首条、事件顺序与
// finish 标记、主题精确过滤（work_id 隔离 + 相近 request_id 不串）、
// PCM 二进制 base64 往返、接收超时与发布端关闭不挂起、协议校验。
#include "voxorchestra/dataplane/dataplane_event.hpp"
#include "voxorchestra/dataplane/event_channel.hpp"
#include "voxorchestra/protocol/message_envelope.hpp"
#include "voxorchestra/transport/pubsub.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>

#include <zmq.hpp>

namespace vd = voxorchestra::dataplane;
namespace et = voxorchestra::transport;
namespace pr = voxorchestra::protocol;
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

  // partial ×3 → final（finish=true）：流内 index 递增，顺序到达。
  vd::DataplaneEvent e;
  for (int i = 0; i < 3; ++i) {
    e.kind = vd::kKindPartial;
    e.text = "partial-" + std::to_string(i);
    e.index = i;
    pub.publish(e, "w-1", "r-1");
  }
  e.kind = vd::kKindFinal;
  e.text = "final-text";
  e.index = 3;
  e.finish = true;
  pub.publish(e, "w-1", "r-1");

  for (int i = 0; i < 3; ++i) {
    vd::DataplaneEvent got;
    CHECK(sub.recv(got, 1000ms));
    CHECK(got.kind == vd::kKindPartial);
    CHECK(got.text == "partial-" + std::to_string(i));
    CHECK(got.index == i);
    CHECK(!got.finish);
  }
  vd::DataplaneEvent got;
  CHECK(sub.recv(got, 1000ms));
  CHECK(got.kind == vd::kKindFinal && got.text == "final-text");
  CHECK(got.index == 3 && got.finish);
  CHECK(!sub.recv(got, 200ms));  // 流已结束，无更多消息
  std::cout << "  [ok] 事件流按 index 顺序到达，finish 标记流尾" << std::endl;
}

void test_topic_isolation() {
  zmq::context_t ctx(1);

  // 两个订阅者各订一条流，发布端混合发布：互不串扰。
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
  e.kind = vd::kKindToken;
  e.index = 0;
  e.text = "a";
  pub.publish(e, "w-1", "r-1");
  e.text = "b";
  pub.publish(e, "w-2", "r-1");
  e.text = "c";
  e.index = 1;
  pub.publish(e, "w-1", "r-1");

  vd::DataplaneEvent got;
  CHECK(sub_a.recv(got, 1000ms));
  CHECK(got.text == "a");
  CHECK(sub_b.recv(got, 1000ms));
  CHECK(got.text == "b");
  CHECK(sub_a.recv(got, 1000ms));
  CHECK(got.text == "c");
  CHECK(!sub_a.recv(got, 200ms));
  CHECK(!sub_b.recv(got, 200ms));
  std::cout << "  [ok] 不同 work_id 的事件流互不串扰" << std::endl;
}

void test_topic_prefix_precision() {
  zmq::context_t ctx(1);

  // 只订 w-1/r-2：相近 request_id（r-20/r-3）必须被过滤。
  vd::EventSubscriber sub(ctx);
  sub.subscribe("w-1", "r-2");
  vd::EventPublisher pub(ctx);
  pub.bind("inproc://dp-prec-data", "inproc://dp-prec-sync");
  sub.connect("inproc://dp-prec-data");
  sub.notify_ready("inproc://dp-prec-sync");
  pub.wait_subscriber_ready(1000ms);

  vd::DataplaneEvent e;
  e.kind = vd::kKindPcm;
  e.index = 0;
  pub.publish(e, "w-1", "r-2");
  pub.publish(e, "w-1", "r-20");
  pub.publish(e, "w-1", "r-3");

  vd::DataplaneEvent got;
  CHECK(sub.recv(got, 1000ms));
  CHECK(!sub.recv(got, 200ms));  // 后两条被精确过滤
  std::cout << "  [ok] 尾斜杠主题约定精确过滤相近 request_id" << std::endl;
}

void test_pcm_roundtrip() {
  // 320 字节 PCM 帧（16kHz/16bit/20ms 帧长）经 base64 往返字节一致。
  std::vector<std::uint8_t> frame(320);
  std::mt19937 rng(42);
  for (auto& b : frame) {
    b = static_cast<std::uint8_t>(rng() & 0xFF);
  }

  zmq::context_t ctx(1);
  vd::EventSubscriber sub(ctx);
  sub.subscribe("w-1", "r-1");
  vd::EventPublisher pub(ctx);
  pub.bind("inproc://dp-pcm-data", "inproc://dp-pcm-sync");
  sub.connect("inproc://dp-pcm-data");
  sub.notify_ready("inproc://dp-pcm-sync");
  pub.wait_subscriber_ready(1000ms);

  vd::DataplaneEvent e;
  e.kind = vd::kKindPcm;
  e.pcm = frame;
  e.index = 7;
  pub.publish(e, "w-1", "r-1");

  vd::DataplaneEvent got;
  CHECK(sub.recv(got, 1000ms));
  CHECK(got.kind == vd::kKindPcm);
  CHECK(got.index == 7);
  CHECK(got.pcm == frame);
  std::cout << "  [ok] PCM 二进制经 base64 编码往返字节一致" << std::endl;
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
  CHECK(!sub.recv(got, 200ms));  // 空流超时返回 false

  pub.close();
  CHECK(!sub.recv(got, 500ms));  // 发布端关闭后不挂起
  std::cout << "  [ok] 空流超时与发布端关闭均快速返回 false" << std::endl;
}

void test_protocol_validation() {
  zmq::context_t ctx(1);

  // 直接经 transport 层向事件主题发布非 event 信封（模拟协议污染）。
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
  test_pcm_roundtrip();
  test_recv_timeout_and_close();
  test_protocol_validation();

  if (g_failures == 0) {
    std::cout << "dataplane_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "dataplane_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
