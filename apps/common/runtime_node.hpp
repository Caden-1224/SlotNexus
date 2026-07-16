// 运行时节点：RpcServer + TaskRuntime 的合体。
//
// 职责：接收 action 信封（setup/inference/cancel/taskinfo/exit），分发到任务
// 状态机，并把结果组装为 ack/error 响应信封。
//
// 具体节点（echo_node / 未来的 asr_node / llm_node / tts_node）只负责注入后端
// 工厂（Echo / RKLLM / sherpa-onnx 等），分发逻辑对所有节点通用。
#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <zmq.hpp>

#include "voxorchestra/runtime/task_runtime.hpp"
#include "voxorchestra/transport/rpc.hpp"

namespace voxorchestra::node {

class RuntimeNode {
 public:
  // runtime：节点的任务运行时（含后端工厂）。
  RuntimeNode(zmq::context_t& ctx, std::unique_ptr<runtime::TaskRuntime> runtime);
  ~RuntimeNode() = default;

  RuntimeNode(const RuntimeNode&) = delete;
  RuntimeNode& operator=(const RuntimeNode&) = delete;

  void bind(const std::string& endpoint);

  // 处理至多一条请求；poll_timeout 内无请求返回 false。
  bool serve_once(std::chrono::milliseconds poll_timeout);

  // 幂等；关闭后 serve_once 抛 kClosed。
  void close();

 private:
  // 全部异常转换为错误信封，保证不向 RpcServer 抛异常。
  std::string handle_request(const std::string& request_json);

  transport::RpcServer server_;
  std::unique_ptr<runtime::TaskRuntime> runtime_;
};

}  // namespace voxorchestra::node
