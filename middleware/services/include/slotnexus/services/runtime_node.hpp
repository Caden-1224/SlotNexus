// 通用节点外壳公共接口。
//
// Author: Caden
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
  RuntimeNode(zmq::context_t& ctx, std::unique_ptr<runtime::TaskRuntime> runtime,
              std::chrono::milliseconds infer_timeout = std::chrono::seconds(120),
              std::shared_ptr<dataplane::EventPublisher> events = nullptr);

  void bind(const std::string& endpoint);
  bool serve_once(std::chrono::milliseconds poll_timeout);
  void close();

 private:
  std::string handle_request(const std::string& request_json);

  transport::RpcServer server_;
  std::unique_ptr<runtime::TaskRuntime> runtime_;
  std::chrono::milliseconds infer_timeout_;
  std::shared_ptr<dataplane::EventPublisher> events_;
};

}  // namespace slotnexus::node
