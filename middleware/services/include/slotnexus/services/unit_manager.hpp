// Unit Manager：通用任务生命周期与模块路由控制面。
//
// Author: Caden
// 设计意图：核心只理解“模块描述符”和节点端点，不写任何业务模块名特判。
// 每个模块有稳定 module_id，可包含一个或多个节点端点；setup 请求通过
// payload.module_id 选择模块，并按模块内端点轮转选择节点；后续 inference/
// cancel/taskinfo/exit 按 work_id 固定路由到首次选择的节点。
//
// 旧请求兼容：payload 未携带 module_id 时选择 default_module_id；若只有
// 一个模块，CLI 可自动把它作为默认模块。
#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <zmq.hpp>

#include "slotnexus/protocol/message_envelope.hpp"
#include "slotnexus/task_registry/task_registry.hpp"
#include "slotnexus/transport/rpc.hpp"

namespace slotnexus::manager {

struct ModuleDescriptor {
  std::string module_id;
  int protocol_version = 1;
  std::vector<std::string> node_endpoints;
  std::string config_path;
};

class UnitManager {
 public:
  UnitManager(zmq::context_t& ctx, std::vector<ModuleDescriptor> modules,
              std::string default_module_id = {},
              std::size_t max_tasks = 0,
              std::chrono::milliseconds node_rpc_deadline =
                  std::chrono::milliseconds(3000));
  ~UnitManager() = default;

  UnitManager(const UnitManager&) = delete;
  UnitManager& operator=(const UnitManager&) = delete;

  void bind(const std::string& endpoint);
  bool serve_once(std::chrono::milliseconds poll_timeout);
  void close();

  const std::vector<ModuleDescriptor>& modules() const { return modules_; }
  const std::string& default_module_id() const { return default_module_id_; }

 private:
  struct Route {
    std::size_t module_index = 0;
    std::size_t node_index = 0;
  };

  std::string handle_request(const std::string& request_json);
  protocol::MessageEnvelope forward(const protocol::MessageEnvelope& request,
                                    const Route& route);
  const ModuleDescriptor* find_module(const std::string& module_id) const;
  std::size_t find_module_index(const std::string& module_id) const;

  transport::RpcServer server_;
  task_registry::TaskRegistry registry_;
  std::vector<ModuleDescriptor> modules_;
  std::string default_module_id_;
  std::vector<std::size_t> next_node_;
  std::vector<std::vector<std::unique_ptr<transport::RpcClient>>> node_clients_;
  std::unordered_map<std::string, Route> route_;
  std::chrono::milliseconds node_rpc_deadline_;
};

}  // namespace slotnexus::manager
