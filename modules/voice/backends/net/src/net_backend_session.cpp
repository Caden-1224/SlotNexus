#include "slotnexus/backend/net/net_backend_session.hpp"
// Author: Caden

#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

#include "slotnexus/common/log.hpp"
#include "slotnexus/transport/transport_error.hpp"
#include "slotnexus/voice/event_adapter.hpp"

namespace slotnexus::backend::net {

namespace {

using protocol::MessageEnvelope;
using protocol::MessageType;
using protocol::ProtocolError;

// 主题约定：<work_id>/<request_id>/（与 dataplane EventPublisher 一致）。
std::string make_topic(const std::string& work_id,
                       const std::string& request_id) {
  return work_id + "/" + request_id + "/";
}

// 数据面事件 → 语音后端事件。
BackendEvent to_backend_event(const dataplane::DataplaneEvent& e) {
  return voice::FromDataplaneEvent(e);
}

}  // namespace

NetBackendSession::NetBackendSession(zmq::context_t& ctx,
                                     NetBackendConfig config)
    : ctx_(ctx),
      config_(std::move(config)),
      rpc_(ctx),
      cancel_rpc_(ctx),
      sub_(ctx) {
  rpc_.connect(config_.rpc_endpoint);
  cancel_rpc_.connect(config_.rpc_endpoint);
  // 订阅先于发布（slow joiner 契约）：连接后即握手，节点端确认订阅就绪。
  const auto handshake_start = std::chrono::steady_clock::now();
  sub_.connect(config_.events_endpoint);
  sub_.notify_ready(config_.events_sync);
  common::LogLine(
      "net handshake work_id=" + config_.work_id + " rpc=" +
      config_.rpc_endpoint + " ms=" +
      std::to_string(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - handshake_start)
              .count()));
}

NetBackendSession::~NetBackendSession() {
  // 取消在途推理并置标志：驱动线程（若有）在下一轮循环退出。
  cancel();
  // 通知节点清理任务（fire-and-forget）：节点可能仍在处理上一推理，
  // exit 请求在 REP 队列中排队其后——FIFO 保证最终执行。节点不可达/
  // 任务已释放时静默忽略（晚到无 ACK 是预期行为）。
  MessageEnvelope req;
  req.set_type(MessageType::kExit);
  req.set_work_id(config_.work_id);
  req.set_request_id(config_.work_id + "#exit");
  try {
    cancel_rpc_.call(req.to_json(), std::chrono::milliseconds(1000));
  } catch (const std::exception&) {
  }
}

void NetBackendSession::set_event_callback(EventCallback cb) {
  // 每次推理开始前调用：清理上一轮 cancel 标志，否则被取消后端在下次
  // 预推理合并/续说重跑时会直接空返回。
  cancelled_.store(false);
  cb_ = std::move(cb);
}

void NetBackendSession::setup() {
  MessageEnvelope req;
  req.set_type(MessageType::kSetup);
  req.set_work_id(config_.work_id);
  req.set_request_id(config_.work_id + "#setup");
  req.set_payload(nlohmann::json::object());
  std::string resp;
  const auto setup_start = std::chrono::steady_clock::now();
  try {
    resp = rpc_.call(req.to_json(), config_.setup_timeout);
  } catch (const transport::TransportError& e) {
    throw std::runtime_error("节点不可达（" + config_.rpc_endpoint + "）: " +
                             e.what());
  }
  common::LogLine(
      "net setup work_id=" + config_.work_id + " rpc=" +
      config_.rpc_endpoint + " ms=" +
      std::to_string(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - setup_start)
              .count()));
  const MessageEnvelope reply = MessageEnvelope::from_json(resp);
  if (reply.type() == MessageType::kError) {
    throw std::runtime_error("节点 setup 失败: " + reply.error().message);
  }
}

void NetBackendSession::cancel() {
  cancelled_.store(true);
  // fire-and-forget：节点 cancel 请求（REP 串行，晚到排队；无 ACK 是
  // 预期行为）。节点不可达/已关闭时静默忽略。
  MessageEnvelope req;
  req.set_type(MessageType::kCancel);
  req.set_work_id(config_.work_id);
  req.set_request_id(config_.work_id + "#cancel");
  try {
    cancel_rpc_.call(req.to_json(), std::chrono::milliseconds(1000));
  } catch (const std::exception&) {
  }
}

std::string NetBackendSession::drive_inference(const std::string& request_id,
                                               const nlohmann::json& payload) {
  // 0. 上一轮未确认完成（取消/超时/异常退出）：REQ 可能停在等待响应
  //    阶段，重建 socket 恢复状态机（驱动线程独占，无并发）。
  if (rpc_pending_) {
    rpc_.connect(config_.rpc_endpoint);
    rpc_pending_ = false;
  }

  // 1. 订阅本轮事件流；订阅传播需要时间（发布端在推理开始时才 publish），
  //    等待 settle 窗口（回环上通常 <10ms，100ms 为稳健余量）。
  const std::string topic = make_topic(config_.work_id, request_id);
  sub_.subscribe(config_.work_id, request_id);
  std::this_thread::sleep_for(config_.subscribe_settle);

  // 2. 发出 inference 请求（不阻塞），进入轮询。
  MessageEnvelope req;
  req.set_type(MessageType::kInference);
  req.set_work_id(config_.work_id);
  req.set_request_id(request_id);
  req.set_payload(payload);
  rpc_.call_async(req.to_json());
  // 请求已发出：此后任何未收到响应的退出（取消/超时/异常）都使 REQ
  // 停在等待阶段，置 pending 供下一轮重建。
  rpc_pending_ = true;

  // 3. 轮询：事件（SUB）实时回放 + 响应（REP）完成确认。
  //    完成以双信号判定：事件流 finish（final/done，数据面权威尾）+ ack
  //    （控制面确认）。节点在事件之后回 ack，但 RPC 直连比 PUB 订阅传播
  //    快，ack 可能先于最后一条事件到达——ack 后保留余量窗口继续回放，
  //    防止丢尾；finish 事件才是流的完成标志。
  const auto deadline =
      std::chrono::steady_clock::now() + config_.rpc_timeout;
  constexpr std::chrono::milliseconds kAckSettle(200);  // ack 后事件传播余量
  std::string ack_text;
  bool ack_done = false;
  bool stream_done = false;
  auto ack_time = std::chrono::steady_clock::time_point{};
  while (!(ack_done && stream_done)) {
    if (cancelled_.load()) {
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      break;
    }
    if (ack_done &&
        std::chrono::steady_clock::now() - ack_time >= kAckSettle) {
      break;  // ack 后余量耗尽（事件流未收全，如订阅竞态丢流）：兜底退出
    }
    dataplane::DataplaneEvent e;
    std::string got_topic;
    while (sub_.recv_with_topic(e, got_topic, std::chrono::milliseconds::zero())) {
      if (got_topic == topic) {
        if (e.finish) {
          stream_done = true;
        }
        if (cb_) {
          cb_(to_backend_event(e));  // 旧流残留（其他主题）丢弃
        }
      }
    }
    if (!ack_done) {
      std::string resp;
      if (rpc_.poll_response(resp, std::chrono::milliseconds(10))) {
        rpc_pending_ = false;  // 响应已收到：REQ 回到可发状态
        const MessageEnvelope reply = MessageEnvelope::from_json(resp);
        if (reply.type() == MessageType::kError) {
          throw std::runtime_error("节点推理失败: " + reply.error().message);
        }
        ack_text = reply.payload().value("text", std::string());
        ack_done = true;
        ack_time = std::chrono::steady_clock::now();
      }
    }
  }

  if (!(ack_done && stream_done)) {
    if (cancelled_.load()) {
      // 取消：会话侧提前返回（节点推理继续但结果被 pipeline 世代过滤丢弃）。
      return {};
    }
    throw std::runtime_error("节点推理超时（" + config_.rpc_endpoint +
                             "，等待 " +
                             std::to_string(config_.rpc_timeout.count()) +
                             "ms 未完成）");
  }
  return ack_text;
}

void NetBackendSession::begin_event_stream(const std::string& request_id) {
  stream_topic_ = make_topic(config_.work_id, request_id);
  sub_.subscribe(config_.work_id, request_id);
  std::this_thread::sleep_for(config_.subscribe_settle);
}

bool NetBackendSession::pump_event_stream(std::chrono::milliseconds timeout,
                                          bool* finished) {
  bool got = false;
  bool first = true;
  dataplane::DataplaneEvent e;
  std::string topic;
  const auto first_timeout = timeout;
  while (sub_.recv_with_topic(e, topic, first ? first_timeout
                                              : std::chrono::milliseconds(0))) {
    first = false;
    if (topic != stream_topic_) {
      continue;  // 旧流/其他流残留
    }
    if (e.finish && finished != nullptr) {
      *finished = true;
    }
    if (cb_) {
      cb_(to_backend_event(e));
    }
    got = true;
    if (finished != nullptr && *finished) {
      break;
    }
  }
  return got;
}

std::string NetBackendSession::next_request_id(const std::string& stage) {
  return stage + std::to_string(seq_.fetch_add(1));
}

}  // namespace slotnexus::backend::net
