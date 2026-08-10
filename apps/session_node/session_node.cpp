#include "session_node.hpp"

#include <atomic>
#include <filesystem>
#include <utility>

#include <nlohmann/json.hpp>

#include "voxorchestra/backend/fake/fake_audio_sink.hpp"
#include "voxorchestra/backend/fake/fake_asr_backend.hpp"
#include "voxorchestra/backend/fake/fake_llm_backend.hpp"
#include "voxorchestra/backend/fake/fake_tts_backend.hpp"
#include "voxorchestra/backend/i_asr_backend.hpp"
#include "voxorchestra/backend/i_llm_backend.hpp"
#include "voxorchestra/backend/i_tts_backend.hpp"
#ifdef VOXORCHESTRA_HAS_ALSA
#include "voxorchestra/backend/alsa/alsa_audio_source.hpp"
#endif
#include "voxorchestra/backend/net/net_asr_backend.hpp"
#include "voxorchestra/backend/net/net_llm_backend.hpp"
#include "voxorchestra/backend/net/net_tts_backend.hpp"
#include "voxorchestra/common/log.hpp"
#include "voxorchestra/protocol/message_envelope.hpp"
#include "voxorchestra/rag/knowledge_store.hpp"

namespace voxorchestra::app {

namespace {

using protocol::MessageEnvelope;
using protocol::MessageType;
using protocol::ProtocolError;
using protocol::ProtocolErrorCode;
using session::PipelineConfig;
using session::PipelineInput;
using session::PipelineResult;

// 工作线程 PUSH 发送超时：close 后管道已关闭时快速放弃，避免挂死。
constexpr int kWorkerPushTimeoutMs = 500;

// 工作线程编号（inproc 端点唯一化）。
std::atomic<std::uint32_t> g_instance_seq{0};

const char* PipelineInputModeName(PipelineInput::Mode mode) {
  switch (mode) {
    case PipelineInput::Mode::kText:
      return "text";
    case PipelineInput::Mode::kWav:
      return "wav";
    case PipelineInput::Mode::kMic:
      return "mic";
  }
  return "unknown";
}

// 结果 → taskinfo/ack 的公共统计字段。
nlohmann::json ResultStats(const PipelineResult& r) {
  return {{"route", r.route},
          {"asr_text", r.asr_text},
          {"final_text", r.final_text},
          {"generation", r.generation},
          {"token_count", r.token_count},
          {"pcm_frames", r.pcm_frames},
          {"text_queue_peak", r.text_queue_peak},
          {"pcm_queue_peak", r.pcm_queue_peak},
          {"dropped_sentences", r.dropped_sentences},
          {"dropped_pcm_frames", r.dropped_pcm_frames}};
}

}  // namespace

// 一个会话实例：后端归本实例所有（embedded=Fake / net=节点代理，见
// MakeAsrBackend 等），管线只依赖接口引用。
struct SessionNode::Session {
  std::unique_ptr<voxorchestra::backend::IAsrBackend> asr;
  std::unique_ptr<voxorchestra::backend::ILlmBackend> llm;
  std::unique_ptr<voxorchestra::backend::ITtsBackend> tts;
  std::unique_ptr<session::SessionPipeline> pipeline;
  std::atomic<bool> busy{false};
  std::mutex last_mutex;
  session::PipelineResult last_result;
  std::string last_request_id;
};

// 后端工厂：按配置模式创建 asr/llm/tts 后端。
//  - embedded：本地 Fake（确定性，M1 基线）；
//  - net：远端节点代理（work_id 与节点任务一致；构造时同步 setup 节点，
//    失败抛异常 → 会话 setup 失败，客户端早失败早清楚）。
std::unique_ptr<voxorchestra::backend::IAsrBackend> MakeAsrBackend(
    zmq::context_t& ctx, const SessionNodeConfig& cfg,
    const std::string& work_id) {
  if (cfg.backend == "net") {
    voxorchestra::backend::net::NetBackendConfig c{
        cfg.asr_ep.rpc, cfg.asr_ep.events, cfg.asr_ep.sync, work_id,
        cfg.net_setup_timeout, cfg.net_rpc_timeout};
    c.asr_audio_uplink = cfg.asr_audio_uplink;
    return std::make_unique<voxorchestra::backend::net::NetAsrBackend>(ctx,
                                                                       c);
  }
  return std::make_unique<voxorchestra::backend::fake::FakeAsrBackend>();
}

std::unique_ptr<voxorchestra::backend::ILlmBackend> MakeLlmBackend(
    zmq::context_t& ctx, const SessionNodeConfig& cfg,
    const std::string& work_id) {
  if (cfg.backend == "net") {
    return std::make_unique<voxorchestra::backend::net::NetLlmBackend>(
        ctx, voxorchestra::backend::net::NetBackendConfig{
                 cfg.llm_ep.rpc, cfg.llm_ep.events, cfg.llm_ep.sync, work_id,
                 cfg.net_setup_timeout, cfg.net_rpc_timeout});
  }
  return std::make_unique<voxorchestra::backend::fake::FakeLlmBackend>();
}

std::unique_ptr<voxorchestra::backend::ITtsBackend> MakeTtsBackend(
    zmq::context_t& ctx, const SessionNodeConfig& cfg,
    const std::string& work_id) {
  if (cfg.backend == "net") {
    return std::make_unique<voxorchestra::backend::net::NetTtsBackend>(
        ctx, voxorchestra::backend::net::NetBackendConfig{
                 cfg.tts_ep.rpc, cfg.tts_ep.events, cfg.tts_ep.sync, work_id,
                 cfg.net_setup_timeout, cfg.net_rpc_timeout});
  }
  return std::make_unique<voxorchestra::backend::fake::FakeTtsBackend>();
}

SessionNode::SessionNode(zmq::context_t& ctx, SessionNodeConfig config)
    : ctx_(ctx), config_(std::move(config)) {
  // 知识库 → BM25 索引 → L0-L3 路由（全节点共享，只读）。
  const rag::KnowledgeStore store(config_.knowledge_path);
  rag::Bm25Index index;
  for (const auto& e : store.entries()) {
    index.add_document(e.text);
  }
  index.build();
  router_ = std::make_unique<rag::Router>(std::move(index), store.entries(),
                                          config_.router);
}

SessionNode::~SessionNode() { close(); }

void SessionNode::bind() {
  router_socket_ =
      std::make_unique<zmq::socket_t>(ctx_, zmq::socket_type::router);
  router_socket_->bind(config_.listen);
  reply_endpoint_ =
      "inproc://session-replies-" + std::to_string(g_instance_seq.fetch_add(1));
  replies_ = std::make_unique<zmq::socket_t>(ctx_, zmq::socket_type::pull);
  replies_->bind(reply_endpoint_);
}

bool SessionNode::serve_once(std::chrono::milliseconds poll_timeout) {
  if (closed_) {
    return false;
  }
  // 经典 zmq_poll：ROUTER 请求 + 工作线程回复两个事件源。
  zmq::pollitem_t items[] = {
      {*router_socket_, 0, ZMQ_POLLIN, 0},
      {*replies_, 0, ZMQ_POLLIN, 0},
  };
  int n = 0;
  try {
    n = zmq::poll(items, 2, static_cast<long>(poll_timeout.count()));
  } catch (const zmq::error_t& e) {
    if (e.num() == EINTR) {
      // 被信号中断（如 SIGTERM 优雅退出）：按无事件返回，调用方重新轮询。
      return false;
    }
    throw;
  }
  if (n <= 0) {
    return false;
  }
  bool handled = false;
  if (items[0].revents & ZMQ_POLLIN) {
    // REQ 客户端发来 [identity, 空分隔, payload]。
    zmq::message_t identity;
    zmq::message_t delim;
    zmq::message_t payload;
    const bool ok = router_socket_->recv(identity, zmq::recv_flags::none) &&
                    router_socket_->recv(delim, zmq::recv_flags::none) &&
                    router_socket_->recv(payload, zmq::recv_flags::none);
    if (ok) {
      handle_request(identity.to_string(), payload.to_string());
      handled = true;
    }
  }
  if (items[1].revents & ZMQ_POLLIN) {
    // 工作线程投递 [identity, 响应 JSON]。
    zmq::message_t identity;
    zmq::message_t reply;
    const bool ok = replies_->recv(identity, zmq::recv_flags::none) &&
                    replies_->recv(reply, zmq::recv_flags::none);
    if (ok) {
      router_socket_->send(identity, zmq::send_flags::sndmore);
      router_socket_->send(zmq::str_buffer(""), zmq::send_flags::sndmore);
      router_socket_->send(reply, zmq::send_flags::none);
      handled = true;
    }
  }
  return handled;
}

void SessionNode::close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  // 取消全部在途推理，让工作线程尽快退出。
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    for (auto& [id, s] : sessions_) {
      s->pipeline->cancel();
    }
  }
  // 等待工作线程退出（管线均有兜底 deadline，不会无限挂起）。
  {
    std::lock_guard<std::mutex> lock(workers_mutex_);
    for (auto& t : workers_) {
      if (t.joinable()) {
        t.join();
      }
    }
    workers_.clear();
  }
  replies_.reset();
  router_socket_.reset();
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.clear();
  }
}

std::size_t SessionNode::session_count() const {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  return sessions_.size();
}

void SessionNode::handle_request(const std::string& identity,
                                 const std::string& request_json) {
  // 同步应答直接发送；失败必须回 error 信封，保证 REQ 客户端不悬挂。
  const auto send_reply = [&](const MessageEnvelope& reply) {
    router_socket_->send(zmq::buffer(identity), zmq::send_flags::sndmore);
    router_socket_->send(zmq::str_buffer(""), zmq::send_flags::sndmore);
    router_socket_->send(zmq::buffer(reply.to_json()), zmq::send_flags::none);
  };
  const auto build_error = [](const MessageEnvelope& req, int code,
                              const std::string& message) {
    MessageEnvelope e;
    e.set_type(MessageType::kError);
    e.set_work_id(req.work_id());
    e.set_request_id(req.request_id());
    e.set_session_id(req.session_id());
    e.set_error({code, message});
    e.set_finish(true);
    return e;
  };

  MessageEnvelope request;
  try {
    request = MessageEnvelope::from_json(request_json);
  } catch (const ProtocolError& e) {
    common::LogLine("session err bad_json frame=" + request_json.substr(0, 80));
    send_reply(build_error(request, static_cast<int>(e.code()), e.what()));
    return;
  }
  // 请求级日志：一次调用的完整路径从这里的 request_id 开始关联。
  common::LogLine("session req request_id=" + request.request_id() + " type=" +
                  protocol::message_type_to_string(request.type()) +
                  " work_id=" + request.work_id());
  // 错误分支统一记日志（门禁 3：请求与响应全程可关联）。
  const auto log_err = [](const MessageEnvelope& req, const std::string& msg) {
    common::LogLine("session err request_id=" + req.request_id() + " " + msg);
  };

  switch (request.type()) {
    case MessageType::kSetup: {
      std::shared_ptr<Session> s;
      {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        if (sessions_.count(request.work_id()) > 0) {
          log_err(request, "duplicate_setup");
          send_reply(build_error(request, 2, "重复 setup"));  // kBadState
          return;
        }
        s = std::make_shared<Session>();
        // 后端创建（net 模式含节点 setup RPC）；失败 → 会话 setup 失败。
        // 错误码 4 = 后端不可用（节点未启动/连接超时）。
        try {
          s->asr = MakeAsrBackend(ctx_, config_, request.work_id());
          s->llm = MakeLlmBackend(ctx_, config_, request.work_id());
          s->tts = MakeTtsBackend(ctx_, config_, request.work_id());
        } catch (const std::exception& e) {
          log_err(request, std::string("backend_unavailable: ") + e.what());
          send_reply(build_error(request, 4,
                                 "后端不可用: " + std::string(e.what())));
          return;
        }
        s->pipeline = std::make_unique<session::SessionPipeline>(
            PipelineConfig{config_.text_capacity, config_.pcm_capacity,
                           config_.push_timeout, config_.stage_delay,
                           config_.output_dir, config_.tts_min_duration},
            *router_, *s->asr, *s->llm, *s->tts,
            [](const std::string& path) {
              return std::make_unique<voxorchestra::backend::fake::FakeAudioSink>(
                  path);
            });
        sessions_[request.work_id()] = s;
      }
      MessageEnvelope ack;
      ack.set_type(MessageType::kAck);
      ack.set_work_id(request.work_id());
      ack.set_request_id(request.request_id());
      ack.set_session_id(request.session_id());
      ack.set_payload({{"status", "ok"}});
      ack.set_finish(true);
      send_reply(ack);
      return;
    }
    case MessageType::kInference: {
      std::shared_ptr<Session> s;
      {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        const auto it = sessions_.find(request.work_id());
        if (it == sessions_.end()) {
          log_err(request, "unknown_work_id");
          send_reply(build_error(request, 1, "未知任务: " + request.work_id()));
          return;
        }
        s = it->second;
      }
      bool expected = false;
      if (!s->busy.compare_exchange_strong(expected, true)) {
        log_err(request, "busy");
        send_reply(build_error(request, 3, "会话忙碌（单流）"));  // kBusy
        return;
      }
      // 解析输入：{"mode": "text"|"wav"|"alsa", ...}。alsa = 现场麦克风
      // 录音（--record-device / --record-ms），样本随管线 kMic 输入。
      PipelineInput input;
      const auto& payload = request.payload();
      const std::string mode = payload.value("mode", "text");
      if (mode == "wav") {
        input.mode = PipelineInput::Mode::kWav;
        std::string wav = payload.value("wav", std::string());
        if (wav.empty()) {
          wav = payload.value("text", std::string());
        }
        // 相对路径按固定输入目录解析（产品代码不写死本机路径）。
        if (!wav.empty() && wav[0] != '/') {
          wav = config_.fixture_dir + "/" + wav;
        }
        input.wav_path = wav;
      } else if (mode == "alsa") {
#ifdef VOXORCHESTRA_HAS_ALSA
        input.mode = PipelineInput::Mode::kMic;
        // 阻塞采集 record_duration 时长：20 ms 帧轮询读取，XRUN 空帧
        // 重试本帧（AlsaAudioSource 契约）。录音中不可取消（单流模型，
        // 请求在途即占用会话），时长由 --record-ms 控制。
        voxorchestra::backend::alsa::AlsaAudioSource mic(
            config_.record_device, voxorchestra::backend::kSampleRateHz);
        if (!mic.open()) {
          log_err(request, "mic_open_failed");
          send_reply(build_error(request, 3,
                                 "录音设备打开失败: " + config_.record_device));
          return;
        }
        const int total_frames = std::max(
            1, static_cast<int>(config_.record_duration.count()) / 20);
        input.audio.reserve(static_cast<std::size_t>(total_frames) *
                            voxorchestra::backend::kFrameSamples);
        for (int i = 0; i < total_frames; ++i) {
          auto chunk = mic.read(voxorchestra::backend::kFrameSamples);
          if (chunk.empty()) {
            --i;  // overrun 空帧：重试本帧
            continue;
          }
          input.audio.insert(input.audio.end(), chunk.begin(), chunk.end());
        }
        mic.close();
#else
        send_reply(build_error(
            request, 3,
            "当前构建未启用 ALSA 录音（需硬件后端构建，--record-device）"));
        return;
#endif
      } else {
        input.mode = PipelineInput::Mode::kText;
        input.text = payload.value("text", std::string());
      }
      const std::string work_id = request.work_id();
      const std::string request_id = request.request_id();
      // 兜底 deadline：防止异常后端无限挂起工作线程。
      const auto deadline = config_.max_run;
      {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers_.emplace_back([this, identity, work_id, request_id, s, input,
                               deadline] {
          run_inference(identity, work_id, request_id, s, input, deadline);
        });
      }
      return;
    }
    case MessageType::kCancel: {
      std::shared_ptr<Session> s;
      {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        const auto it = sessions_.find(request.work_id());
        if (it == sessions_.end()) {
          log_err(request, "unknown_work_id");
          send_reply(build_error(request, 1, "未知任务: " + request.work_id()));
          return;
        }
        s = it->second;
      }
      s->pipeline->cancel();  // 异步：递增 generation 并传播到后端
      MessageEnvelope ack;
      ack.set_type(MessageType::kAck);
      ack.set_work_id(request.work_id());
      ack.set_request_id(request.request_id());
      ack.set_session_id(request.session_id());
      ack.set_payload({{"status", "ok"}});
      ack.set_finish(true);
      send_reply(ack);
      return;
    }
    case MessageType::kTaskInfo: {
      std::shared_ptr<Session> s;
      {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        const auto it = sessions_.find(request.work_id());
        if (it == sessions_.end()) {
          log_err(request, "unknown_work_id");
          send_reply(build_error(request, 1, "未知任务: " + request.work_id()));
          return;
        }
        s = it->second;
      }
      PipelineResult last;
      std::string last_request_id;
      {
        std::lock_guard<std::mutex> lock(s->last_mutex);
        last = s->last_result;
        last_request_id = s->last_request_id;
      }
      MessageEnvelope ack;
      ack.set_type(MessageType::kAck);
      ack.set_work_id(request.work_id());
      ack.set_request_id(request.request_id());
      ack.set_session_id(request.session_id());
      nlohmann::json p = ResultStats(last);
      p["state"] = s->pipeline->state_name();
      p["busy"] = s->busy.load();
      p["in_flight"] = last_request_id;
      ack.set_payload(std::move(p));
      ack.set_finish(true);
      send_reply(ack);
      return;
    }
    case MessageType::kExit: {
      std::shared_ptr<Session> s;
      {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        const auto it = sessions_.find(request.work_id());
        if (it == sessions_.end()) {
          log_err(request, "unknown_work_id");
          send_reply(build_error(request, 1, "未知任务: " + request.work_id()));
          return;
        }
        s = it->second;
        sessions_.erase(it);
      }
      s->pipeline->cancel();  // 在途推理立即作废（对象由工作线程 shared_ptr 持有）
      MessageEnvelope ack;
      ack.set_type(MessageType::kAck);
      ack.set_work_id(request.work_id());
      ack.set_request_id(request.request_id());
      ack.set_session_id(request.session_id());
      ack.set_payload({{"status", "ok"}});
      ack.set_finish(true);
      send_reply(ack);
      return;
    }
    default:
      log_err(request, "invalid_type");
      send_reply(build_error(request,
                             static_cast<int>(ProtocolErrorCode::kInvalidType),
                             "session_node 不支持该消息类型"));
      return;
  }
}

void SessionNode::run_inference(const std::string& identity,
                                const std::string& work_id,
                                const std::string& request_id,
                                std::shared_ptr<Session> s,
                                const PipelineInput& input,
                                std::chrono::milliseconds deadline) {
  // 工作线程专用 PUSH：投递完成后即销毁（每请求一个 socket）。
  zmq::socket_t push(ctx_, zmq::socket_type::push);
  push.set(zmq::sockopt::sndtimeo, kWorkerPushTimeoutMs);
  push.connect(reply_endpoint_);

  common::LogLine("session run request_id=" + request_id + " work_id=" + work_id +
                  " mode=" + PipelineInputModeName(input.mode));
  const PipelineResult result = s->pipeline->run(input, request_id, deadline);

  {
    std::lock_guard<std::mutex> lock(s->last_mutex);
    s->last_result = result;
    s->last_request_id = request_id;
  }
  s->busy.store(false);

  MessageEnvelope reply;
  reply.set_type(MessageType::kAck);
  reply.set_work_id(work_id);
  reply.set_request_id(request_id);
  reply.set_session_id("");
  nlohmann::json p = ResultStats(result);
  p["wav_path"] = result.wav_path;
  std::string status;
  if (result.cancelled) {
    status = "cancelled";
  } else if (result.ok) {
    status = "ok";
  } else {
    status = "error";
    p["error"] = result.error;
  }
  p["status"] = status;
  common::LogLine("session done request_id=" + request_id + " work_id=" + work_id +
                  " status=" + status + " route=" + result.route +
                  " tokens=" + std::to_string(result.token_count) +
                  " pcm=" + std::to_string(result.pcm_frames) +
                  (result.error.empty() ? "" : " error=" + result.error) +
                  (result.wav_path.empty()
                       ? ""
                       : " wav=" + result.wav_path));
  reply.set_payload(std::move(p));
  reply.set_finish(true);
  try {
    push.send(zmq::buffer(identity), zmq::send_flags::sndmore);
    push.send(zmq::buffer(reply.to_json()), zmq::send_flags::none);
  } catch (const zmq::error_t&) {
    // close 后管道不可用：丢弃（进程正在退出）。
  }
}

}  // namespace voxorchestra::app
