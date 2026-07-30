#include "unit_manager.hpp"

#include <utility>

#include "action_helpers.hpp"
#include "voxorchestra/common/log.hpp"
#include "voxorchestra/protocol/message_envelope.hpp"
#include "voxorchestra/runtime/task_channel.hpp"
#include "voxorchestra/transport/transport_error.hpp"

namespace voxorchestra::manager {

using protocol::MessageEnvelope;
using protocol::MessageType;
using protocol::ProtocolError;
using protocol::ProtocolErrorCode;
using runtime::TaskChannel;

UnitManager::UnitManager(zmq::context_t& ctx,
                         std::vector<std::string> node_endpoints,
                         std::size_t max_tasks,
                         std::chrono::milliseconds node_rpc_deadline)
    : server_(ctx), registry_(max_tasks),
      node_endpoints_(std::move(node_endpoints)),
      node_rpc_deadline_(node_rpc_deadline) {
  for (const auto& endpoint : node_endpoints_) {
    auto client = std::make_unique<transport::RpcClient>(ctx);
    client->connect(endpoint);
    node_clients_.push_back(std::move(client));
  }
}

void UnitManager::bind(const std::string& endpoint) { server_.bind(endpoint); }

bool UnitManager::serve_once(std::chrono::milliseconds poll_timeout) {
  return server_.serve_once_timeout(
      [this](const std::string& request) { return handle_request(request); },
      poll_timeout);
}

void UnitManager::close() {
  server_.close();
  for (auto& client : node_clients_) {
    client->close();
  }
}

std::string UnitManager::handle_request(const std::string& request_json) {
  MessageEnvelope request;
  try {
    request = MessageEnvelope::from_json(request_json);
  } catch (const ProtocolError& e) {
    common::LogLine("mgr err bad_json frame=" + request_json.substr(0, 80));
    return app::BuildError(request, static_cast<int>(e.code()), e.what()).to_json();
  }
  // 请求级日志：request_id 贯穿 manager 转发全程（门禁 3）。
  common::LogLine("mgr req request_id=" + request.request_id() + " type=" +
                  protocol::message_type_to_string(request.type()) +
                  " work_id=" + request.work_id());

  switch (request.type()) {
    case MessageType::kSetup: {
      const std::string work_id = registry_.allocate();
      if (work_id.empty()) {
        common::LogLine("mgr err request_id=" + request.request_id() +
                        " capacity_exhausted");
        return app::BuildError(request,
                               static_cast<int>(TaskChannel::Error::kCapacity),
                               "任务容量已耗尽")
            .to_json();
      }
      // 把 Manager 全局分配的 work_id 写入信封再转发。
      request.set_work_id(work_id);
      const std::size_t node_index = next_node_++ % node_endpoints_.size();
      route_[work_id] = node_index;
      common::LogLine("mgr alloc request_id=" + request.request_id() +
                      " work_id=" + work_id + " node=" +
                      std::to_string(node_index));
      const MessageEnvelope reply = forward(request, node_index);
      if (reply.type() == MessageType::kError) {
        route_.erase(work_id);  // setup 失败不占用路由与名额
        registry_.release(work_id);
      }
      return reply.to_json();
    }
    case MessageType::kInference:
    case MessageType::kCancel:
    case MessageType::kTaskInfo:
    case MessageType::kExit: {
      const auto it = route_.find(request.work_id());
      if (it == route_.end()) {
        common::LogLine("mgr err request_id=" + request.request_id() +
                        " unknown_work_id=" + request.work_id());
        return app::BuildError(request,
                               static_cast<int>(TaskChannel::Error::kNotExist),
                               "未知任务: " + request.work_id())
            .to_json();
      }
      const MessageEnvelope reply = forward(request, it->second);
      // exit 成功：任务生命周期结束，清理路由并释放 work_id。
      if (request.type() == MessageType::kExit &&
          reply.type() == MessageType::kAck) {
        route_.erase(request.work_id());
        registry_.release(request.work_id());
      }
      return reply.to_json();
    }
    default:
      // 防御：网关只转发五种 action；其余类型在此拒绝。
      common::LogLine("mgr err request_id=" + request.request_id() +
                      " invalid_type");
      return app::BuildError(
                 request, static_cast<int>(ProtocolErrorCode::kInvalidType),
                 "Manager 不支持该消息类型: " +
                     protocol::message_type_to_string(request.type()))
          .to_json();
  }
}

MessageEnvelope UnitManager::forward(const MessageEnvelope& request,
                                     std::size_t node_index) {
  try {
    const std::string reply_json =
        node_clients_[node_index]->call(request.to_json(), node_rpc_deadline_);
    const MessageEnvelope reply = MessageEnvelope::from_json(reply_json);
    common::LogLine("mgr reply request_id=" + request.request_id() + " type=" +
                    protocol::message_type_to_string(reply.type()));
    return reply;
  } catch (const ProtocolError& e) {
    common::LogLine("mgr err request_id=" + request.request_id() +
                    " bad_reply");
    return app::BuildError(request, static_cast<int>(e.code()), e.what());
  } catch (const transport::TransportError& e) {
    // 节点不可达：错误信封回给网关；setup 路径由调用方清理路由与名额。
    common::LogLine("mgr err request_id=" + request.request_id() +
                    " node_unreachable");
    return app::BuildError(request, -1, "node_unreachable: " + std::string(e.what()));
  }
}

}  // namespace voxorchestra::manager
