// 数据面事件通道：节点侧发布（PUB）+ 会话侧订阅（SUB），带订阅握手。
//
// 主题约定：<work_id>/<request_id>/（尾斜杠保证前缀过滤精确，避免
// "w-1/r-2" 误收 "w-1/r-20" 的事件流）。消息体为统一信封
// （MessageEnvelope，type=kEvent），载荷含 kind/text/pcm(base64)。
// 握手复用 transport 的 PubSocket/SubSocket：订阅端先订阅再连接，
// notify_ready 后发布端才开始 publish，保证首条事件不丢。
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

#include <zmq.hpp>

#include "voxorchestra/dataplane/dataplane_event.hpp"
#include "voxorchestra/transport/pubsub.hpp"

namespace voxorchestra::dataplane {

// 发布端：绑定数据端点与握手端点，按 (work_id, request_id) 发布事件流。
class EventPublisher {
 public:
  explicit EventPublisher(zmq::context_t& ctx) : pub_(ctx) {}
  ~EventPublisher() { close(); }

  EventPublisher(const EventPublisher&) = delete;
  EventPublisher& operator=(const EventPublisher&) = delete;

  void bind(const std::string& data_endpoint,
            const std::string& sync_endpoint);
  void wait_subscriber_ready(std::chrono::milliseconds timeout);
  // 发布一条事件。index 语义：e.index >= 0 时按调用方指定；< 0（默认，
  // 如后端事件映射）时按 <work_id>/<request_id> 流自动从 0 递增。
  // 自动递增的状态在发布端维护（单线程使用，与推理线程一致）。
  void publish(const DataplaneEvent& e, const std::string& work_id,
               const std::string& request_id);
  void close();  // 幂等

 private:
  transport::PubSocket pub_;
  std::unordered_map<std::string, int64_t> next_index_;
};

// 订阅端：可订阅多条事件流（每次 subscribe 一个 (work_id, request_id)）。
class EventSubscriber {
 public:
  explicit EventSubscriber(zmq::context_t& ctx) : sub_(ctx) {}
  ~EventSubscriber() { close(); }

  EventSubscriber(const EventSubscriber&) = delete;
  EventSubscriber& operator=(const EventSubscriber&) = delete;

  void subscribe(const std::string& work_id, const std::string& request_id);
  void connect(const std::string& data_endpoint);
  void notify_ready(const std::string& sync_endpoint);
  // 接收并解码一条事件；超时或通道关闭返回 false；
  // 消息不是合法事件（类型不符/缺字段/非法 base64）抛 ProtocolError。
  bool recv(DataplaneEvent& e, std::chrono::milliseconds timeout);

  // 接收事件并返回其来源主题（<work_id>/<request_id>/）。
  // 订阅端可据此精确过滤：本订阅者可能持有多条流的订阅（会话侧网络后端
  // 每轮推理订阅新主题），取消/超时后旧流残留事件仍会到达，按主题丢弃。
  bool recv_with_topic(DataplaneEvent& e, std::string& topic,
                       std::chrono::milliseconds timeout);
  void close();  // 幂等

 private:
  transport::SubSocket sub_;
};

}  // namespace voxorchestra::dataplane
