#include "runtime_node.hpp"

#include <utility>

#include "action_helpers.hpp"
#include "voxorchestra/protocol/message_envelope.hpp"

namespace voxorchestra::node {

using protocol::MessageEnvelope;
using protocol::MessageType;
using protocol::ProtocolError;
using protocol::ProtocolErrorCode;
using runtime::TaskChannel;

RuntimeNode::RuntimeNode(
    zmq::context_t& ctx, std::unique_ptr<runtime::TaskRuntime> runtime,
    std::chrono::milliseconds infer_timeout,
    std::shared_ptr<dataplane::EventPublisher> events)
    : server_(ctx),
      runtime_(std::move(runtime)),
      infer_timeout_(infer_timeout),
      events_(std::move(events)) {}

void RuntimeNode::bind(const std::string& endpoint) { server_.bind(endpoint); }

bool RuntimeNode::serve_once(std::chrono::milliseconds poll_timeout) {
  return server_.serve_once_timeout(
      [this](const std::string& request) { return handle_request(request); },
      poll_timeout);
}

void RuntimeNode::close() { server_.close(); }

std::string RuntimeNode::handle_request(const std::string& request_json) {
  MessageEnvelope request;
  try {
    request = MessageEnvelope::from_json(request_json);
  } catch (const ProtocolError& e) {
    return app::BuildError(request, static_cast<int>(e.code()), e.what()).to_json();
  }

  switch (request.type()) {
    case MessageType::kSetup: {
      const auto r = runtime_->setup_with(request.work_id(), request.request_id(),
                                          request.payload().dump());
      if (r.error != TaskChannel::Error::kOk) {
        return app::BuildError(request, static_cast<int>(r.error),
                               runtime::to_string(r.error))
            .to_json();
      }
      return app::BuildAck(request, {{"status", "ok"}}).to_json();
    }
    case MessageType::kInference: {
      std::string out;
      // 事件出口：把后端流式事件实时发布到数据面（主题 work_id/request_id）。
      // 无发布器时 sink 为空，后端事件被忽略（行为与之前一致）。
      runtime::EventSink sink;
      if (events_) {
        sink = [this, work_id = request.work_id(),
                request_id = request.request_id()](
                   const backend::BackendEvent& e) {
          events_->publish(dataplane::dataplane_event_from_backend(e), work_id,
                           request_id);
        };
      }
      // 0 → 默认超时；返回 kTimeout/kCancelled 时 out 无产出。
      const TaskChannel::Error err = runtime_->inference(
          request.work_id(), request.request_id(), app::ExtractText(request.payload()),
          infer_timeout_, &out, sink);
      if (err != TaskChannel::Error::kOk) {
        return app::BuildError(request, static_cast<int>(err),
                               runtime::to_string(err))
            .to_json();
      }
      return app::BuildAck(request, {{"text", out}}).to_json();
    }
    case MessageType::kCancel: {
      const TaskChannel::Error err =
          runtime_->cancel(request.work_id(), request.request_id());
      if (err != TaskChannel::Error::kOk) {
        return app::BuildError(request, static_cast<int>(err),
                               runtime::to_string(err))
            .to_json();
      }
      return app::BuildAck(request, {{"status", "ok"}}).to_json();
    }
    case MessageType::kTaskInfo: {
      const auto info = runtime_->taskinfo(request.work_id());
      if (!info.has_value()) {
        return app::BuildError(request, static_cast<int>(TaskChannel::Error::kNotExist),
                               "未知任务: " + request.work_id())
            .to_json();
      }
      return app::BuildAck(
                 request,
                 {{"state", runtime::to_string(info->state)},
                  {"in_flight", info->in_flight},
                  {"inference_count", info->inference_count},
                  {"setup_payload", info->setup_payload}})
          .to_json();
    }
    case MessageType::kExit: {
      const TaskChannel::Error err = runtime_->exit(request.work_id());
      if (err != TaskChannel::Error::kOk) {
        return app::BuildError(request, static_cast<int>(err),
                               runtime::to_string(err))
            .to_json();
      }
      return app::BuildAck(request, {{"status", "ok"}}).to_json();
    }
    default:
      // 防御：节点只处理五种 action；其余类型由网关/Manager 过滤。
      return app::BuildError(
                 request, static_cast<int>(ProtocolErrorCode::kInvalidType),
                 "节点不支持该消息类型: " + protocol::message_type_to_string(request.type()))
          .to_json();
  }
}

}  // namespace voxorchestra::node
