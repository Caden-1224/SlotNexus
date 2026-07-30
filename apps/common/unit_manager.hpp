// Unit Manager：任务生命周期与节点路由的控制面进程核心。
//
// 职责：
//   - 用 TaskRegistry 全局分配 work_id（跨节点唯一、不回收复用）；
//   - 维护 work_id → 节点 路由表，setup 时轮转选择节点；
//   - 把 action 信封转发给目标节点，并回传节点的响应（ack/error）；
//   - exit 成功或节点失败时清理路由表并释放 work_id。
#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <zmq.hpp>

#include "voxorchestra/protocol/message_envelope.hpp"
#include "voxorchestra/task_registry/task_registry.hpp"
#include "voxorchestra/transport/rpc.hpp"

namespace voxorchestra::manager {

class UnitManager {
 public:
  // node_endpoints：可用节点 RPC 端点列表（至少 1 个）。
  // node_rpc_deadline：转发到节点的 RPC 等待上限（默认 3000 ms；硬件后端
  // 模型加载可能数秒，经 CLI 调大）。
  UnitManager(zmq::context_t& ctx, std::vector<std::string> node_endpoints,
              std::size_t max_tasks = 0,
              std::chrono::milliseconds node_rpc_deadline =
                  std::chrono::milliseconds(3000));
  ~UnitManager() = default;

  UnitManager(const UnitManager&) = delete;
  UnitManager& operator=(const UnitManager&) = delete;

  void bind(const std::string& endpoint);

  // 处理至多一条请求；poll_timeout 内无请求返回 false。
  bool serve_once(std::chrono::milliseconds poll_timeout);

  // 幂等；关闭后 serve_once 抛 kClosed。
  void close();

 private:
  // 全部异常转换为错误信封，保证不向 RpcServer 抛异常。
  std::string handle_request(const std::string& request_json);

  // 转发到指定节点；节点不可达或响应非法时返回错误信封。
  protocol::MessageEnvelope forward(const protocol::MessageEnvelope& request,
                                    std::size_t node_index);

  transport::RpcServer server_;
  task_registry::TaskRegistry registry_;
  std::vector<std::string> node_endpoints_;
  std::vector<std::unique_ptr<transport::RpcClient>> node_clients_;
  std::unordered_map<std::string, std::size_t> route_;  // work_id → 节点下标
  std::chrono::milliseconds node_rpc_deadline_;
  std::size_t next_node_ = 0;
};

}  // namespace voxorchestra::manager
