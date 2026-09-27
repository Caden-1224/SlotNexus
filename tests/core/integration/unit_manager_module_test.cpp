// UnitManager 模块标识选择与 work_id 固定路由测试。
//
// Author: Caden
// 测试目标：
//   1. setup 按 payload.module_id 选择对应模块实例；
//   2. 后续 inference 按 work_id 固定路由，不重新轮询模块列表；
//   3. 未知 module_id 返回结构化 unknown_module 错误；
//   4. 旧语音请求不携带 module_id 时按 default_module_id 兼容路由。
//
// 为避免引入第二个业务模块，测试内使用两个最小 REP 测试替身；
// 它们仅回显模块/node 身份，不是项目模块。
#include "slotnexus/protocol/message_envelope.hpp"
#include "slotnexus/transport/rpc.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <zmq.hpp>

#include "unit_manager.hpp"

namespace {

using slotnexus::manager::ModuleDescriptor;
using slotnexus::manager::UnitManager;
using slotnexus::protocol::MessageEnvelope;
using slotnexus::protocol::MessageType;
using slotnexus::transport::RpcClient;
using slotnexus::transport::RpcServer;
using namespace std::chrono_literals;

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #cond   \
                << std::endl;                                                \
    }                                                                        \
  } while (0)

std::string MakeAck(const std::string& request_json, const std::string& node_id) {
  MessageEnvelope request = MessageEnvelope::from_json(request_json);
  MessageEnvelope reply;
  reply.set_type(MessageType::kAck);
  reply.set_work_id(request.work_id());
  reply.set_request_id(request.request_id());
  reply.set_session_id(request.session_id());
  reply.set_payload({{"node", node_id}, {"work_id", request.work_id()}});
  reply.set_finish(true);
  return reply.to_json();
}

struct TestRpcNode {
  TestRpcNode(zmq::context_t& ctx, std::string endpoint, std::string node_id)
      : server(ctx), endpoint_(std::move(endpoint)), node_id_(std::move(node_id)) {
    server.bind(endpoint_);
  }

  void Start() {
    thread_ = std::thread([this] {
      while (!stop.load()) {
        server.serve_once_timeout(
            [this](const std::string& req) { return MakeAck(req, node_id_); },
            50ms);
      }
    });
  }

  void Stop() {
    stop.store(true);
    if (thread_.joinable()) {
      thread_.join();
    }
    server.close();
  }

  RpcServer server;
  std::string endpoint_;
  std::string node_id_;
  std::atomic<bool> stop{false};
  std::thread thread_;
};

MessageEnvelope Call(RpcClient& client, MessageEnvelope request) {
  const std::string reply_json = client.call(request.to_json(), 2000ms);
  return MessageEnvelope::from_json(reply_json);
}

MessageEnvelope Setup(const std::string& request_id,
                      const nlohmann::json& payload) {
  MessageEnvelope request;
  request.set_type(MessageType::kSetup);
  request.set_request_id(request_id);
  request.set_payload(payload);
  return request;
}

MessageEnvelope Inference(const std::string& work_id,
                          const std::string& request_id) {
  MessageEnvelope request;
  request.set_type(MessageType::kInference);
  request.set_work_id(work_id);
  request.set_request_id(request_id);
  request.set_payload({{"text", "hello"}});
  return request;
}

}  // namespace

int main() {
  std::cout << "unit_manager_module_test:" << std::endl;

  zmq::context_t ctx(1);
  TestRpcNode alpha(ctx, "inproc://um-module-alpha", "alpha");
  TestRpcNode beta(ctx, "inproc://um-module-beta", "beta");
  alpha.Start();
  beta.Start();

  std::vector<ModuleDescriptor> modules = {
      {"alpha", 1, {"inproc://um-module-alpha"}, ""},
      {"beta", 1, {"inproc://um-module-beta"}, ""},
  };
  UnitManager manager(ctx, modules, "alpha");
  manager.bind("inproc://um-module-front");

  std::atomic<bool> stop{false};
  std::thread manager_thread([&] {
    while (!stop.load()) {
      manager.serve_once(50ms);
    }
  });

  RpcClient client(ctx);
  client.connect("inproc://um-module-front");

  // 1. 显式选择 beta，后续 inference 按 work_id 固定回到 beta。
  const MessageEnvelope setup_beta =
      Call(client, Setup("s-beta", {{"module_id", "beta"}, {"text", "x"}}));
  CHECK(setup_beta.type() == MessageType::kAck);
  CHECK(setup_beta.payload().value("node", "") == "beta");
  const std::string beta_work = setup_beta.work_id();
  CHECK(!beta_work.empty());
  const MessageEnvelope infer_beta =
      Call(client, Inference(beta_work, "r-beta"));
  CHECK(infer_beta.type() == MessageType::kAck);
  CHECK(infer_beta.payload().value("node", "") == "beta");
  std::cout << "  [ok] module_id=beta 选择 beta，后续 work_id 固定路由" << std::endl;

  // 2. 不携带 module_id 的旧语音请求按默认模块 alpha 兼容路由。
  const MessageEnvelope setup_legacy =
      Call(client, Setup("s-legacy", {{"text", "你好"}}));
  CHECK(setup_legacy.type() == MessageType::kAck);
  CHECK(setup_legacy.payload().value("node", "") == "alpha");
  const MessageEnvelope infer_legacy =
      Call(client, Inference(setup_legacy.work_id(), "r-legacy"));
  CHECK(infer_legacy.type() == MessageType::kAck);
  CHECK(infer_legacy.payload().value("node", "") == "alpha");
  std::cout << "  [ok] 无 module_id 旧请求按 default_module_id 兼容" << std::endl;

  // 3. 未知模块：结构化错误，且不分配 work_id。
  const MessageEnvelope unknown =
      Call(client, Setup("s-unknown", {{"module_id", "voice-typo"}}));
  CHECK(unknown.type() == MessageType::kError);
  CHECK(unknown.error().message.find("unknown_module") != std::string::npos);
  CHECK(unknown.work_id().empty());
  std::cout << "  [ok] 未知 module_id 返回 unknown_module 错误" << std::endl;

  // 4. 模块选择后再次 setup beta：仍按 payload.module_id 选择，不因前序
  //    work_id 路由而漂移。
  const MessageEnvelope setup_beta2 =
      Call(client, Setup("s-beta-2", {{"module_id", "beta"}}));
  CHECK(setup_beta2.type() == MessageType::kAck);
  CHECK(setup_beta2.payload().value("node", "") == "beta");
  std::cout << "  [ok] 新 setup 重新按 module_id 选择模块实例" << std::endl;

  stop.store(true);
  manager_thread.join();
  manager.close();
  client.close();
  alpha.Stop();
  beta.Stop();

  if (g_failures == 0) {
    std::cout << "unit_manager_module_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "unit_manager_module_test 失败 " << g_failures << " 项"
            << std::endl;
  return 1;
}
