// Session 故障注入测试（Mock 冻结回归）：异常后端、在途推理中 exit/cancel、
// 忙时拒绝、空闲取消、缺失 WAV、挂起后端 max_run 兜底超时、重复 cancel/exit。
//
// 进程内运行 SessionNode（服务线程 + RpcClient 直连），不 fork 子进程，
// 故障场景可精确编排：
//   1. 后端抛异常：管线返回错误结果而非崩溃，状态机回到 Idle；
//   2. 在途推理中 exit：推理以 cancelled 收尾，同 work_id 可重新 setup；
//   3. 在途推理中 cancel：推理以 cancelled 收尾；
//   4. 忙时第二次推理：明确 kBusy 拒绝；
//   5. 空闲 cancel：ack 且后续推理正常；
//   6. 缺失 WAV：明确错误；
//   7. 挂起后端：max_run 兜底超时（管线层），状态回 Idle 且管线可复用；
//   8. max_run 节点层：工作线程超时收尾，busy 复位，后续推理正常；
//   9. 重复 cancel：均 ack（幂等），会话存活；
//   10. 重复 exit：首次 ack、再次结构化错误，同 work_id 可重建。
#include "voxorchestra/protocol/message_envelope.hpp"
#include "session_node.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <zmq.hpp>

#include <unistd.h>  // chdir

#include "voxorchestra/backend/backend_event.hpp"
#include "voxorchestra/backend/fake/fake_audio_sink.hpp"
#include "voxorchestra/backend/fake/fake_asr_backend.hpp"
#include "voxorchestra/backend/fake/fake_llm_backend.hpp"
#include "voxorchestra/backend/fake/fake_tts_backend.hpp"
#include "voxorchestra/rag/router.hpp"
#include "voxorchestra/session/session_pipeline.hpp"

using namespace std::chrono_literals;

namespace ep = voxorchestra::protocol;
using ep::MessageEnvelope;
using ep::MessageType;
namespace app = voxorchestra::app;
namespace back = voxorchestra::backend;
namespace fake = voxorchestra::backend::fake;
namespace rg = voxorchestra::rag;
namespace sess = voxorchestra::session;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      ++g_failures;                                                          \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #cond   \
                << std::endl;                                                \
    }                                                                        \
  } while (0)

// 抛异常的大模型后端：模拟真实 SDK 在生成中崩溃。
class ThrowingLlm final : public back::ILlmBackend {
 public:
  void set_event_callback(back::EventCallback) override {}
  void generate(const std::string&) override {
    throw std::runtime_error("模拟后端崩溃");
  }
  void cancel() override {}
};

// 直接 REQ 客户端（短超时）。
class ReqClient {
 public:
  explicit ReqClient(zmq::context_t& ctx) : socket_(ctx, zmq::socket_type::req) {
    socket_.set(zmq::sockopt::sndtimeo, 3000);
    socket_.set(zmq::sockopt::rcvtimeo, 3000);
    socket_.set(zmq::sockopt::linger, 0);
  }
  void connect(const std::string& endpoint) { socket_.connect(endpoint); }
  bool call(const MessageEnvelope& req, MessageEnvelope& reply,
            std::chrono::milliseconds timeout) {
    socket_.set(zmq::sockopt::rcvtimeo, static_cast<int>(timeout.count()));
    try {
      socket_.send(zmq::buffer(req.to_json()), zmq::send_flags::none);
      zmq::message_t msg;
      const bool got = socket_.recv(msg, zmq::recv_flags::none).has_value();
      if (!got) {
        return false;
      }
      reply = MessageEnvelope::from_json(msg.to_string());
      return true;
    } catch (...) {
      return false;
    }
  }

 private:
  zmq::socket_t socket_;
};

MessageEnvelope MakeRequest(MessageType type, const std::string& work_id,
                            const std::string& request_id,
                            nlohmann::json payload = nlohmann::json::object()) {
  MessageEnvelope e;
  e.set_type(type);
  e.set_work_id(work_id);
  e.set_request_id(request_id);
  e.set_payload(std::move(payload));
  return e;
}

// 进程内 SessionNode 夹具：服务线程驱动 serve_once，close 时退出。
struct NodeFixture {
  zmq::context_t ctx{1};
  std::unique_ptr<app::SessionNode> node;
  std::thread server;

  NodeFixture() {
    app::SessionNodeConfig config;
    config.listen = "tcp://127.0.0.1:19220";  // 与 e2e/demo 错开
    config.knowledge_path = "data/knowledge/knowledge.jsonl";
    config.output_dir = "/tmp/vox-fault-out";
    config.stage_delay = 20ms;  // 拉长推理窗口便于故障编排
    std::filesystem::remove_all(config.output_dir);
    node = std::make_unique<app::SessionNode>(ctx, config);
    node->bind();
    server = std::thread([this] {
      while (!closed) {
        node->serve_once(50ms);
      }
      node->close();
    });
  }

  std::atomic<bool> closed{false};

  ~NodeFixture() {
    closed = true;
    server.join();
  }
};

// ---------- 1. 后端抛异常：管线不崩溃、错误结果、状态回 Idle ----------

void test_backend_throws() {
  rg::Bm25Index index;  // 空知识库
  index.build();
  rg::Router router(index, {}, rg::RouterConfig{});
  fake::FakeAsrBackend asr;
  ThrowingLlm llm;
  fake::FakeTtsBackend tts;
  sess::PipelineConfig cfg;
  cfg.output_dir = "/tmp/vox-fault-out";
  std::filesystem::create_directories(cfg.output_dir);
  sess::SessionPipeline pipe(
      cfg, router, asr, llm, tts, [](const std::string& p) {
        return std::make_unique<fake::FakeAudioSink>(p);
      });
  const auto r = pipe.run({sess::PipelineInput::Mode::kText, "hello", ""},
                          "req-throw", 0ms);
  CHECK(!r.ok);
  CHECK(!r.cancelled);
  CHECK(r.error.find("管线异常") != std::string::npos);
  CHECK(pipe.state_name() == std::string("idle"));
  // 异常后管线仍可复用。
  const auto r2 = pipe.run({sess::PipelineInput::Mode::kText, "hello", ""},
                           "req-throw-2", 0ms);
  CHECK(!r2.ok);  // 后端持续抛异常，但每次都有明确错误结果
  std::cout << "  [ok] 后端抛异常：错误结果、状态回 Idle、无崩溃" << std::endl;
}

// ---------- 2. 在途推理中 exit：cancelled 收尾、同 work_id 可重建 ----------

void test_exit_during_inference() {
  NodeFixture f;
  ReqClient a(f.ctx), b(f.ctx);
  a.connect(f.node->listen_endpoint());
  b.connect(f.node->listen_endpoint());

  MessageEnvelope reply;
  CHECK(a.call(MakeRequest(MessageType::kSetup, "w-x", "s-x"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kAck);

  MessageEnvelope r_infer;
  std::thread infer_thread([&] {
    a.call(MakeRequest(MessageType::kInference, "w-x", "r-x",
                       {{"mode", "text"},
                        {"text",
                         "你好 今天天气 明天 后天 开心 快乐 轻松 愉快 阳光 "
                         "微风 散步 唱歌 跳舞 画画 读书 写字 下棋 钓鱼 爬山"}}),
           r_infer, 5000ms);
  });
  std::this_thread::sleep_for(150ms);
  CHECK(b.call(MakeRequest(MessageType::kExit, "w-x", "e-x"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kAck);
  infer_thread.join();
  CHECK(r_infer.payload().value("status", std::string()) == "cancelled");
  // 已释放：taskinfo 返回 not_exist。
  CHECK(!b.call(MakeRequest(MessageType::kTaskInfo, "w-x", "t-x"), reply, 3000ms) ||
        reply.type() == MessageType::kError);
  // 同 work_id 可重新 setup。
  CHECK(b.call(MakeRequest(MessageType::kSetup, "w-x", "s-x2"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kAck);
  CHECK(b.call(MakeRequest(MessageType::kInference, "w-x", "r-x2",
                           {{"mode", "text"}, {"text", "16kHz 音频格式"}}),
               reply, 3000ms));
  CHECK(reply.payload().value("status", std::string()) == "ok");
  std::cout << "  [ok] 在途推理中 exit：cancelled 收尾、同 work_id 可重建"
            << std::endl;
}

// ---------- 3. 在途推理中 cancel：cancelled 收尾 ----------

void test_cancel_during_inference() {
  NodeFixture f;
  ReqClient a(f.ctx), b(f.ctx);
  a.connect(f.node->listen_endpoint());
  b.connect(f.node->listen_endpoint());
  MessageEnvelope reply;
  CHECK(a.call(MakeRequest(MessageType::kSetup, "w-c", "s-c"), reply, 3000ms));

  MessageEnvelope r_infer;
  std::thread infer_thread([&] {
    a.call(MakeRequest(MessageType::kInference, "w-c", "r-c",
                       {{"mode", "text"},
                        {"text",
                         "你好 今天天气 明天 后天 开心 快乐 轻松 愉快 阳光 "
                         "微风 散步 唱歌 跳舞 画画 读书 写字 下棋 钓鱼 爬山"}}),
           r_infer, 5000ms);
  });
  std::this_thread::sleep_for(150ms);
  CHECK(b.call(MakeRequest(MessageType::kCancel, "w-c", "c-c"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kAck);
  infer_thread.join();
  CHECK(r_infer.payload().value("status", std::string()) == "cancelled");
  CHECK(r_infer.payload().value("pcm_frames", 999) == 0);
  // 会话仍存活，可继续推理。
  CHECK(b.call(MakeRequest(MessageType::kInference, "w-c", "r-c2",
                           {{"mode", "text"}, {"text", "16kHz 音频格式"}}),
               reply, 3000ms));
  CHECK(reply.payload().value("status", std::string()) == "ok");
  std::cout << "  [ok] 在途推理中 cancel：cancelled 收尾、会话可复用" << std::endl;
}

// ---------- 4. 忙时第二次推理：明确 kBusy 拒绝 ----------

void test_busy_reject() {
  NodeFixture f;
  ReqClient a(f.ctx), b(f.ctx);
  a.connect(f.node->listen_endpoint());
  b.connect(f.node->listen_endpoint());
  MessageEnvelope reply;
  CHECK(a.call(MakeRequest(MessageType::kSetup, "w-b", "s-b"), reply, 3000ms));

  std::thread infer_thread([&] {
    a.call(MakeRequest(MessageType::kInference, "w-b", "r-b",
                       {{"mode", "text"},
                        {"text",
                         "你好 今天天气 明天 后天 开心 快乐 轻松 愉快 阳光 "
                         "微风 散步 唱歌 跳舞 画画 读书 写字 下棋 钓鱼 爬山"}}),
           reply, 5000ms);
  });
  std::this_thread::sleep_for(100ms);
  MessageEnvelope busy_reply;
  CHECK(b.call(MakeRequest(MessageType::kInference, "w-b", "r-b2",
                           {{"mode", "text"}, {"text", "x"}}),
               busy_reply, 3000ms));
  CHECK(busy_reply.type() == MessageType::kError);
  CHECK(busy_reply.error().code == 3);  // kBusy
  infer_thread.join();
  std::cout << "  [ok] 忙时第二次推理：明确 kBusy 拒绝" << std::endl;
}

// ---------- 5. 空闲 cancel：ack 且后续推理正常 ----------

void test_cancel_while_idle() {
  NodeFixture f;
  ReqClient a(f.ctx);
  a.connect(f.node->listen_endpoint());
  MessageEnvelope reply;
  CHECK(a.call(MakeRequest(MessageType::kSetup, "w-i", "s-i"), reply, 3000ms));
  CHECK(a.call(MakeRequest(MessageType::kCancel, "w-i", "c-i"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kAck);  // 空操作但明确应答
  CHECK(a.call(MakeRequest(MessageType::kInference, "w-i", "r-i",
                           {{"mode", "text"}, {"text", "16kHz 音频格式"}}),
               reply, 3000ms));
  CHECK(reply.payload().value("status", std::string()) == "ok");
  std::cout << "  [ok] 空闲 cancel：ack 应答，后续推理正常" << std::endl;
}

// ---------- 6. 缺失 WAV：明确错误 ----------

void test_missing_wav() {
  NodeFixture f;
  ReqClient a(f.ctx);
  a.connect(f.node->listen_endpoint());
  MessageEnvelope reply;
  CHECK(a.call(MakeRequest(MessageType::kSetup, "w-w", "s-w"), reply, 3000ms));
  CHECK(a.call(MakeRequest(MessageType::kInference, "w-w", "r-w",
                           {{"mode", "wav"}, {"wav", "no-such-file.wav"}}),
               reply, 3000ms));
  CHECK(reply.payload().value("status", std::string()) == "error");
  CHECK(reply.payload().value("error", std::string()).find("无法打开") !=
        std::string::npos);
  std::cout << "  [ok] 缺失 WAV：明确错误结果" << std::endl;
}

// ---------- 7. 挂起后端：max_run 兜底超时（管线层） ----------

// 长时间无响应的后端：不回调任何事件，模拟真实 SDK 有限挂起。
class HungLlm final : public back::ILlmBackend {
 public:
  void set_event_callback(back::EventCallback) override {}
  void generate(const std::string&) override {
    std::this_thread::sleep_for(2s);  // 远大于下面的兜底 deadline
  }
  void cancel() override {}
};

void test_max_run_timeout_hung_backend() {
  rg::Bm25Index index;
  index.build();
  rg::Router router(index, {}, rg::RouterConfig{});
  fake::FakeAsrBackend asr;
  HungLlm llm;
  fake::FakeTtsBackend tts;
  sess::PipelineConfig cfg;
  cfg.output_dir = "/tmp/vox-fault-out";
  std::filesystem::create_directories(cfg.output_dir);
  sess::SessionPipeline pipe(cfg, router, asr, llm, tts,
                             [](const std::string& p) {
                               return std::make_unique<fake::FakeAudioSink>(p);
                             });
  // deadline=300ms << 后端 2s 挂起：生成返回后在阶段边界命中兜底超时。
  const auto r = pipe.run({sess::PipelineInput::Mode::kText, "hello", ""},
                          "req-hung", 300ms);
  CHECK(!r.ok);
  CHECK(!r.cancelled);
  CHECK(r.error.find("管线超时") != std::string::npos);
  CHECK(pipe.state_name() == std::string("idle"));
  // 超时后管线仍可复用（同一挂起后端，每次都明确超时收尾而非崩溃）。
  const auto r2 = pipe.run({sess::PipelineInput::Mode::kText, "hello", ""},
                           "req-hung-2", 300ms);
  CHECK(r2.error.find("管线超时") != std::string::npos);
  std::cout << "  [ok] 挂起后端：max_run 兜底超时、状态回 Idle、管线可复用"
            << std::endl;
}

// ---------- 8. max_run 节点层：工作线程超时收尾、busy 复位 ----------

void test_max_run_node_level() {
  zmq::context_t ctx(1);
  app::SessionNodeConfig config;
  config.listen = "tcp://127.0.0.1:19222";  // 与同文件 19220 夹具错开
  config.knowledge_path = "data/knowledge/knowledge.jsonl";
  config.output_dir = "/tmp/vox-fault-out";
  config.stage_delay = 50ms;  // 放大 LLM 生成时长
  config.max_run = 200ms;     // 兜底上限远小于一次完整生成
  std::filesystem::remove_all(config.output_dir);
  auto node = std::make_unique<app::SessionNode>(ctx, config);
  node->bind();
  std::atomic<bool> closed{false};
  std::thread server([&] {
    while (!closed) {
      node->serve_once(50ms);
    }
    node->close();
  });

  ReqClient a(ctx);
  a.connect(node->listen_endpoint());
  MessageEnvelope reply;
  CHECK(a.call(MakeRequest(MessageType::kSetup, "w-t", "s-t"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kAck);
  // 长文本 ~19 token × 50ms ≈ 950ms 生成，200ms 后必然命中兜底超时。
  CHECK(a.call(MakeRequest(MessageType::kInference, "w-t", "r-t",
                           {{"mode", "text"},
                            {"text",
                             "你好 今天天气 明天 后天 开心 快乐 轻松 愉快 阳光 "
                             "微风 散步 唱歌 跳舞 画画 读书 写字 下棋 钓鱼 爬山"}}),
               reply, 5000ms));
  CHECK(reply.payload().value("status", std::string()) == "error");
  CHECK(reply.payload().value("error", std::string()).find("管线超时") !=
        std::string::npos);
  // 工作线程已退出、busy 复位：taskinfo 回到 idle，后续推理正常。
  CHECK(a.call(MakeRequest(MessageType::kTaskInfo, "w-t", "t-t"), reply, 3000ms));
  CHECK(reply.payload().value("state", std::string()) == "idle");
  CHECK(reply.payload().value("busy", true) == false);
  CHECK(a.call(MakeRequest(MessageType::kInference, "w-t", "r-t2",
                           {{"mode", "text"}, {"text", "16kHz 音频格式"}}),
               reply, 5000ms));
  CHECK(reply.payload().value("status", std::string()) == "ok");

  closed = true;
  server.join();
  std::cout << "  [ok] max_run 节点层：超时收尾、busy 复位、后续推理正常"
            << std::endl;
}

// ---------- 9. 重复 cancel：幂等 ack，会话存活 ----------

void test_duplicate_cancel() {
  NodeFixture f;
  ReqClient a(f.ctx);
  a.connect(f.node->listen_endpoint());
  MessageEnvelope reply;
  CHECK(a.call(MakeRequest(MessageType::kSetup, "w-dc", "s-dc"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kAck);
  CHECK(a.call(MakeRequest(MessageType::kCancel, "w-dc", "c-dc1"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kAck);
  CHECK(a.call(MakeRequest(MessageType::kCancel, "w-dc", "c-dc2"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kAck);  // 幂等：空会话取消仍明确应答
  CHECK(a.call(MakeRequest(MessageType::kInference, "w-dc", "r-dc",
                           {{"mode", "text"}, {"text", "16kHz 音频格式"}}),
               reply, 3000ms));
  CHECK(reply.payload().value("status", std::string()) == "ok");
  std::cout << "  [ok] 重复 cancel：两次均 ack（幂等），会话存活" << std::endl;
}

// ---------- 10. 重复 exit：首次 ack、再次结构化错误，同 work_id 可重建 ----------

void test_duplicate_exit() {
  NodeFixture f;
  ReqClient a(f.ctx);
  a.connect(f.node->listen_endpoint());
  MessageEnvelope reply;
  CHECK(a.call(MakeRequest(MessageType::kSetup, "w-de", "s-de"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kAck);
  CHECK(f.node->session_count() == 1);
  CHECK(a.call(MakeRequest(MessageType::kExit, "w-de", "e-de1"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kAck);
  CHECK(f.node->session_count() == 0);
  // 重复 exit：任务已释放，返回结构化错误而非崩溃。
  CHECK(a.call(MakeRequest(MessageType::kExit, "w-de", "e-de2"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kError);
  CHECK(reply.error().message.find("未知任务") != std::string::npos);
  // 同 work_id 可重新 setup（释放后重建）。
  CHECK(a.call(MakeRequest(MessageType::kSetup, "w-de", "s-de2"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kAck);
  CHECK(a.call(MakeRequest(MessageType::kExit, "w-de", "e-de3"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kAck);
  std::cout << "  [ok] 重复 exit：首次 ack、再次结构化错误、同 work_id 可重建"
            << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
  {
    std::string base = argc > 0 ? argv[0] : ".";
    const auto slash = base.find_last_of('/');
    base = (slash == std::string::npos) ? "." : base.substr(0, slash);
    ::chdir((base + "/../../..").c_str());  // 配置/fixture 相对仓库根
  }
  std::cout << "session_fault_test:" << std::endl;
  test_backend_throws();
  test_exit_during_inference();
  test_cancel_during_inference();
  test_busy_reject();
  test_cancel_while_idle();
  test_missing_wav();
  test_max_run_timeout_hung_backend();
  test_max_run_node_level();
  test_duplicate_cancel();
  test_duplicate_exit();

  if (g_failures == 0) {
    std::cout << "session_fault_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "session_fault_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
