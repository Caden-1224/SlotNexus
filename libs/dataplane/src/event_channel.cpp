// 数据面事件通道实现：主题拼接 + 信封编解码，复用 transport 的 PUB/SUB。
#include "voxorchestra/dataplane/event_channel.hpp"

namespace voxorchestra::dataplane {

namespace {

// 主题约定：<work_id>/<request_id>/，尾斜杠保证前缀过滤精确。
std::string make_topic(const std::string& work_id,
                       const std::string& request_id) {
  return work_id + "/" + request_id + "/";
}

}  // namespace

void EventPublisher::bind(const std::string& data_endpoint,
                          const std::string& sync_endpoint) {
  pub_.bind(data_endpoint);
  pub_.bind_sync(sync_endpoint);
}

void EventPublisher::wait_subscriber_ready(std::chrono::milliseconds timeout) {
  pub_.wait_subscriber_ready(timeout);
}

void EventPublisher::publish(const DataplaneEvent& e,
                             const std::string& work_id,
                             const std::string& request_id) {
  const std::string topic = make_topic(work_id, request_id);
  int64_t index = e.index;
  if (index < 0) {
    // 调用方未指定序号（后端事件映射的默认形态）：按流自动递增。
    index = next_index_[topic];
    next_index_[topic] = index + 1;
  }
  DataplaneEvent ev = e;
  ev.index = index;
  pub_.publish(topic,
               dataplane_event_to_envelope(ev, work_id, request_id).to_json());
}

void EventPublisher::close() { pub_.close(); }

void EventSubscriber::subscribe(const std::string& work_id,
                                const std::string& request_id) {
  sub_.subscribe(make_topic(work_id, request_id));
}

void EventSubscriber::connect(const std::string& data_endpoint) {
  sub_.connect(data_endpoint);
}

void EventSubscriber::notify_ready(const std::string& sync_endpoint) {
  sub_.notify_ready(sync_endpoint);
}

bool EventSubscriber::recv(DataplaneEvent& e,
                           std::chrono::milliseconds timeout) {
  std::string topic;
  return recv_with_topic(e, topic, timeout);
}

bool EventSubscriber::recv_with_topic(DataplaneEvent& e, std::string& topic,
                                      std::chrono::milliseconds timeout) {
  std::string payload;
  if (!sub_.recv(topic, payload, timeout)) {
    return false;
  }
  e = dataplane_event_from_envelope(
      protocol::MessageEnvelope::from_json(payload));
  return true;
}

void EventSubscriber::close() { sub_.close(); }

}  // namespace voxorchestra::dataplane
