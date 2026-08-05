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

#include "voxorchestra/dataplane/event_channel.hpp"
#include "voxorchestra/runtime/task_runtime.hpp"
#include "voxorchestra/transport/rpc.hpp"

namespace voxorchestra::node {

class RuntimeNode {
 public:
  // runtime：节点的任务运行时（含后端工厂）。
  // infer_timeout：单次推理任务的节点内超时；0 表示默认
  // （kDefaultInferenceTimeout 5000 ms）。硬件后端推理可达数十秒
  // （RKLLM 板端 ~7 tok/s），需节点经 --infer-timeout-ms 调大。
  // events：可选数据面事件发布器（节点启动时绑定端点并注入）。
  // 非空时推理过程中的流式后端事件（partial/token/PCM）实时发布到
  // 数据面，主题 <work_id>/<request_id>/；空则无事件出口（行为不变）。
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
  // 全部异常转换为错误信封，保证不向 RpcServer 抛异常。
  std::string handle_request(const std::string& request_json);

  transport::RpcServer server_;
  std::unique_ptr<runtime::TaskRuntime> runtime_;
  std::chrono::milliseconds infer_timeout_;
  std::shared_ptr<dataplane::EventPublisher> events_;
};

}  // namespace voxorchestra::node
