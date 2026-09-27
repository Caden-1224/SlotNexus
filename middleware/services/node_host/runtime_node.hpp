// 通用节点外壳：RpcServer + TaskRuntime 的组合。
//
// Author: Caden
// 设计意图：节点只负责 action 分发、任务状态持久化和通用事件转发，
// 不解释 payload.text 等应用字段。语音/其他模块通过注入 IBackend 工厂
// 完成各自的负载解释。
#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <zmq.hpp>

#include "slotnexus/dataplane/event_channel.hpp"
#include "slotnexus/runtime/task_runtime.hpp"
#include "slotnexus/transport/rpc.hpp"

namespace slotnexus::node {

class RuntimeNode {
 public:
  // runtime：节点的任务运行时（含后端工厂）。
  // infer_timeout：单次推理任务的节点内超时；0 表示默认 5000 ms。
  // events：可选数据面事件发布器；非空时把 runtime::BackendEvent 原样
  // 转发为 dataplane::DataplaneEvent，不做业务语义解释。
  RuntimeNode(zmq::context_t& ctx, std::unique_ptr<runtime::TaskRuntime> runtime,
              std::chrono::milliseconds infer_timeout = std::chrono::milliseconds(0),
              std::shared_ptr<dataplane::EventPublisher> events = nullptr);
  ~RuntimeNode() = default;

  RuntimeNode(const RuntimeNode&) = delete;
  RuntimeNode& operator=(const RuntimeNode&) = delete;

  void bind(const std::string& endpoint);

  // 处理至多一条请求；poll_timeout 内无请求返回 false。
  bool serve_once(std::chrono::milliseconds poll_timeout);

  // 幂等；关闭后 serve_once 抛 kClosed。
  void close();

 private:
  std::string handle_request(const std::string& request_json);

  transport::RpcServer server_;
  std::unique_ptr<runtime::TaskRuntime> runtime_;
  std::chrono::milliseconds infer_timeout_;
  std::shared_ptr<dataplane::EventPublisher> events_;
};

}  // namespace slotnexus::node
