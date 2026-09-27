// Unit Manager 实现。
//
// Author: Caden
// 入口：构造函数、serve_once/handle_request、forward。
// 关键边界：
//   - setup 先解析 module_id，未知模块返回 unknown_module，不创建 work_id；
//   - 同一 work_id 的后续动作复用 Route，不重新轮转；
//   - 旧请求未携带 module_id 时使用 default_module_id 兼容规则。
#include "unit_manager.hpp"

#include <stdexcept>
#include <utility>

#include "action_helpers.hpp"
#include "slotnexus/common/log.hpp"
#include "slotnexus/protocol/message_envelope.hpp"
#include "slotnexus/runtime/task_channel.hpp"
#include "slotnexus/transport/transport_error.hpp"

namespace slotnexus::manager {

namespace {
constexpr int kUnknownModuleCode = -2;
constexpr int kInvalidModuleConfigCode = -3;
}  // namespace

using protocol::MessageEnvelope;
using protocol::MessageType;
using protocol::ProtocolError;
using protocol::ProtocolErrorCode;
using runtime::TaskChannel;

UnitManager::UnitManager(zmq::context_t& ctx,
                         std::vector<ModuleDescriptor> modules,
                         std::string default_module_id,
                         std::size_t max_tasks,
                         std::chrono::milliseconds node_rpc_deadline)
    : server_(ctx), registry_(max_tasks),
      modules_(std::move(modules)),
      default_module_id_(std::move(default_module_id)),
      node_rpc_deadline_(node_rpc_deadline) {
  if (modules_.empty()) {
    throw std::invalid_argument("UnitManager 至少需要一个模块描述符");
  }
  for (const auto& module : modules_) {
    if (module.module_id.empty() || module.node_endpoints.empty()) {
      throw std::invalid_argument(
          "模块描述符需要非空 module_id 和至少一个节点端点");
    }
  }
  if (default_module_id_.empty()) {
    default_module_id_ = modules_.front().module_id;
  }
  if (find_module_index(default_module_id_) == modules_.size()) {
    throw std::invalid_argument("default_module_id 不在模块列表中");
  }

  next_node_.assign(modules_.size(), 0);
  node_clients_.resize(modules_.size());
  for (std::size_t mi = 0; mi < modules_.size(); ++mi) {
    for (const auto& endpoint : modules_[mi].node_endpoints) {
      auto client = std::make_unique<transport::RpcClient>(ctx);
      client->connect(endpoint);
      node_clients_[mi].push_back(std::move(client));
    }
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
  for (auto& module_clients : node_clients_) {
    for (auto& client : module_clients) {
      client->close();
    }
  }
}

const ModuleDescriptor* UnitManager::find_module(
    const std::string& module_id) const {
  const std::size_t index = find_module_index(module_id);
  return index == modules_.size() ? nullptr : &modules_[index];
}

std::size_t UnitManager::find_module_index(
    const std::string& module_id) const {
  for (std::size_t i = 0; i < modules_.size(); ++i) {
    if (modules_[i].module_id == module_id) {
      return i;
    }
  }
  return modules_.size();
}

std::string UnitManager::handle_request(const std::string& request_json) {
  MessageEnvelope request;
  try {
    request = MessageEnvelope::from_json(request_json);
  } catch (const ProtocolError& e) {
    common::LogLine("mgr err bad_json frame=" + request_json.substr(0, 80));
    return app::BuildError(request, static_cast<int>(e.code()), e.what())
        .to_json();
  }
  common::LogLine("mgr req request_id=" + request.request_id() + " type=" +
                  protocol::message_type_to_string(request.type()) +
                  " work_id=" + request.work_id());

  switch (request.type()) {
    case MessageType::kSetup: {
      std::string module_id = default_module_id_;
      if (request.payload().is_object() && request.payload().contains("module_id")) {
        if (!request.payload()["module_id"].is_string()) {
          return app::BuildError(request, kInvalidModuleConfigCode,
                                 "module_id 必须为字符串")
              .to_json();
        }
        module_id = request.payload()["module_id"].get<std::string>();
      }
      const std::size_t module_index = find_module_index(module_id);
      if (module_index == modules_.size()) {
        common::LogLine("mgr err request_id=" + request.request_id() +
                        " unknown_module=" + module_id);
        return app::BuildError(request, kUnknownModuleCode,
                               "unknown_module: " + module_id)
            .to_json();
      }

      const std::string work_id = registry_.allocate();
      if (work_id.empty()) {
        common::LogLine("mgr err request_id=" + request.request_id() +
                        " capacity_exhausted");
        return app::BuildError(request,
                               static_cast<int>(TaskChannel::Error::kCapacity),
                               "任务容量已耗尽")
            .to_json();
      }
      request.set_work_id(work_id);
      const std::size_t node_index =
          next_node_[module_index]++ % modules_[module_index].node_endpoints.size();
      const Route route{module_index, node_index};
      route_.emplace(work_id, route);
      common::LogLine("mgr alloc request_id=" + request.request_id() +
                      " work_id=" + work_id + " module=" + module_id +
                      " node=" + std::to_string(node_index));
      const MessageEnvelope reply = forward(request, route);
      if (reply.type() == MessageType::kError) {
        route_.erase(work_id);
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
      if (request.type() == MessageType::kExit &&
          reply.type() == MessageType::kAck) {
        route_.erase(request.work_id());
        registry_.release(request.work_id());
      }
      return reply.to_json();
    }
    default:
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
                                     const Route& route) {
  try {
    auto& client = node_clients_[route.module_index][route.node_index];
    const std::string reply_json =
        client->call(request.to_json(), node_rpc_deadline_);
    const MessageEnvelope reply = MessageEnvelope::from_json(reply_json);
    common::LogLine("mgr reply request_id=" + request.request_id() + " type=" +
                    protocol::message_type_to_string(reply.type()));
    return reply;
  } catch (const ProtocolError& e) {
    common::LogLine("mgr err request_id=" + request.request_id() +
                    " bad_reply");
    return app::BuildError(request, static_cast<int>(e.code()), e.what());
  } catch (const transport::TransportError& e) {
    common::LogLine("mgr err request_id=" + request.request_id() +
                    " node_unreachable");
    return app::BuildError(request, -1, "node_unreachable: " +
                                           std::string(e.what()));
  }
}

}  // namespace slotnexus::manager
