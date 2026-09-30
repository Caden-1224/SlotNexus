#include "session_node.hpp"
// Author: Caden

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "slotnexus/backend/fake/fake_audio_sink.hpp"
#include "slotnexus/backend/fake/fake_asr_backend.hpp"
#include "slotnexus/backend/fake/fake_llm_backend.hpp"
#include "slotnexus/backend/fake/fake_tts_backend.hpp"
#include "slotnexus/backend/i_asr_backend.hpp"
#include "slotnexus/backend/i_llm_backend.hpp"
#include "slotnexus/backend/i_tts_backend.hpp"
#ifdef SLOTNEXUS_HAS_ALSA
#include "slotnexus/backend/alsa/alsa_audio_sink.hpp"
#include "slotnexus/backend/alsa/alsa_audio_source.hpp"
#endif
#ifdef SLOTNEXUS_HAS_SHERTA_ONNX
#include "slotnexus/backend/sherpa_onnx/sherpa_kws.hpp"
#include "slotnexus/backend/sherpa_onnx/sherpa_vad.hpp"
#endif
#include "slotnexus/backend/net/net_asr_backend.hpp"
#include "slotnexus/backend/net/net_asr_stream_backend.hpp"
#include "slotnexus/backend/net/net_llm_backend.hpp"
#include "slotnexus/backend/net/net_tts_backend.hpp"
#include "slotnexus/common/log.hpp"
#include "slotnexus/protocol/message_envelope.hpp"
#include "slotnexus/rag/knowledge_store.hpp"
#include "slotnexus/session/streaming_input.hpp"
#include "slotnexus/session/wake_gate.hpp"

namespace slotnexus::app {

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
          {"dropped_pcm_frames", r.dropped_pcm_frames},
          {"output_mode", r.output_mode},
          {"audio_delivered", r.audio_delivered},
          {"wav_complete", r.wav_complete},
          {"playback_complete", r.playback_complete},
          {"sink_error", r.sink_error},
          {"timings_ms",
           {{"input_end", r.input_end_ms},
            {"asr_final", r.asr_final_ms},
            {"llm_first_token", r.llm_first_token_ms},
            {"first_text", r.first_text_ms},
            {"tts_first_pcm", r.tts_first_pcm_ms},
            {"first_output", r.first_output_ms},
            {"output_complete", r.output_complete_ms},
            {"total", r.total_ms}}}};
}

}  // namespace

// 一个会话实例：后端归本实例所有（embedded=Fake / net=节点代理，见
// MakeAsrBackend 等），管线只依赖接口引用。
struct SessionNode::Session {
  ~Session() { StopWakeInput(); }

  std::unique_ptr<slotnexus::backend::IAsrBackend> asr;
  std::unique_ptr<slotnexus::backend::ILlmBackend> llm;
  std::unique_ptr<slotnexus::backend::ITtsBackend> tts;
  std::unique_ptr<session::SessionPipeline> pipeline;
  std::atomic<bool> busy{false};
  long long setup_ms = -1;
  long long asr_setup_ms = -1;
  long long llm_setup_ms = -1;
  long long tts_setup_ms = -1;
  std::mutex last_mutex;
  session::PipelineResult last_result;
  std::string last_request_id;

  // 唤醒期常驻输入：KWS/ASR 在采集线程，SessionPipeline 在单轮处理线程。
  std::atomic<bool> wake_enabled{false};
  std::atomic<bool> wake_available{false};
  mutable std::mutex wake_meta_mutex;
  std::string wake_error;
  std::unique_ptr<session::StreamingInput> stream_input;
  session::WakeGate wake_gate;
#ifdef SLOTNEXUS_HAS_SHERTA_ONNX
  std::unique_ptr<slotnexus::backend::sherpa_onnx::SherpaKws> kws;
  std::shared_ptr<slotnexus::backend::sherpa_onnx::SherpaVad> vad;
#endif
  std::atomic<bool> input_stop{true};
  std::thread capture_thread;
  std::thread turn_thread;
  std::mutex turn_mutex;
  std::condition_variable turn_cv;
  std::deque<std::string> pending_turns;
  std::atomic<int> wake_turns{0};
  int turn_sequence = 0;

  std::string WakeError() const {
    std::lock_guard<std::mutex> lock(wake_meta_mutex);
    return wake_error;
  }

  void SetWakeError(std::string error) {
    std::lock_guard<std::mutex> lock(wake_meta_mutex);
    wake_error = std::move(error);
  }

  std::string WakeState() const {
    if (stream_input) {
      return wake_gate.state_name();
    }
    if (wake_enabled.load()) {
      return "unavailable";
    }
    return "disabled";
  }

  bool WakeInputActive() const {
    return stream_input != nullptr && !input_stop.load();
  }

  void EnqueueTurn(std::string text) {
    if (text.empty() || input_stop.load()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(turn_mutex);
      pending_turns.push_back(std::move(text));
    }
    wake_turns.fetch_add(1);
    turn_cv.notify_one();
  }

  void LogWakeState(const std::string& work_id, const char* event) {
    common::LogLine("session wake state=" +
                    std::string(wake_gate.state_name()) + " work_id=" +
                    work_id + " event=" + event);
  }

  void ResetKws() {
#ifdef SLOTNEXUS_HAS_SHERTA_ONNX
    if (kws) {
      kws->reset();
    }
#endif
  }

  void FeedWakeFrame(const std::int16_t* pcm, std::size_t count,
                     const std::string& work_id) {
#ifdef SLOTNEXUS_HAS_SHERTA_ONNX
    if (kws && wake_gate.state() == session::WakeGate::State::kSleeping) {
      const std::string keyword = kws->accept(pcm, count);
      if (wake_gate.accepts_wake_word(keyword) && wake_gate.wake(keyword)) {
        LogWakeState(work_id, "kws");
      }
    }
#else
    (void)pcm;
    (void)count;
    (void)work_id;
#endif
    stream_input->feed_audio(pcm, count);
  }

  void TickWake(const std::string& work_id) {
    if (!wake_gate.tick()) {
      return;
    }
    if (wake_gate.state() == session::WakeGate::State::kSleeping) {
      ResetKws();
    }
    LogWakeState(work_id, "tick");
  }

  void StopWakeInput() {
    input_stop.store(true);
    if (asr) {
      asr->cancel();
    }
    if (pipeline) {
      pipeline->cancel();
    }
    turn_cv.notify_all();
    if (capture_thread.joinable()) {
      capture_thread.join();
    }
    if (asr) {
      asr->cancel();  // 捕获线程退出前可能在 final 等待中重新激活流
    }
    if (turn_thread.joinable()) {
      turn_thread.join();
    }
    {
      std::lock_guard<std::mutex> lock(turn_mutex);
      pending_turns.clear();
    }
    stream_input.reset();
    ResetKws();
#ifdef SLOTNEXUS_HAS_SHERTA_ONNX
    kws.reset();
#endif
    wake_available.store(false);
  }

  void TurnLoop(SessionNodeConfig cfg, std::string work_id) {
    while (!input_stop.load()) {
      std::string text;
      {
        std::unique_lock<std::mutex> lock(turn_mutex);
        turn_cv.wait(lock, [this] {
          return input_stop.load() || !pending_turns.empty();
        });
        if (input_stop.load() && pending_turns.empty()) {
          break;
        }
        text = std::move(pending_turns.front());
        pending_turns.pop_front();
      }

      bool acquired = false;
      while (!input_stop.load()) {
        bool expected = false;
        if (busy.compare_exchange_weak(expected, true)) {
          acquired = true;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (!acquired) {
        break;
      }

      session::PipelineInput input;
      input.mode = session::PipelineInput::Mode::kText;
      input.text = text;
      const std::string request_id =
          "wake-" + std::to_string(++turn_sequence);
      common::LogLine("session wake turn request_id=" + request_id +
                      " work_id=" + work_id + " text=" + text);
      const session::PipelineResult result =
          pipeline->run(input, request_id, cfg.max_run);
      {
        std::lock_guard<std::mutex> lock(last_mutex);
        last_result = result;
        last_request_id = request_id;
      }
      busy.store(false);
      wake_gate.finish_turn();
      common::LogLine(
          "session wake done request_id=" + request_id + " work_id=" + work_id +
          " status=" + (result.ok ? std::string("ok")
                                  : (result.cancelled ? std::string("cancelled")
                                                      : std::string("error"))) +
          " route=" + result.route +
          " asr_text=" + result.asr_text +
          " total_ms=" + std::to_string(result.total_ms) +
          (result.error.empty() ? "" : " error=" + result.error));
    }
  }

  bool StartWakeInput(const SessionNodeConfig& cfg,
                      const std::string& work_id) {
    wake_enabled.store(cfg.wake_enabled);
    if (!cfg.wake_enabled) {
      return false;
    }

#ifdef SLOTNEXUS_HAS_SHERTA_ONNX
    if (cfg.wake_model_dir.empty() || cfg.wake_keywords_file.empty()) {
      SetWakeError("未配置 KWS 模型目录或关键词文件");
      common::LogLine("session wake unavailable work_id=" + work_id +
                      " reason=" + WakeError());
      return false;
    }
    slotnexus::backend::sherpa_onnx::KwsConfig kws_config;
    kws_config.model_dir = cfg.wake_model_dir;
    kws_config.keywords_file = cfg.wake_keywords_file;
    kws_config.keywords_score = cfg.wake_score;
    kws_config.keywords_threshold = cfg.wake_threshold;
    kws_config.num_trailing_blanks = cfg.wake_trailing_blanks;
    kws_config.num_threads = cfg.wake_num_threads;
    kws = std::make_unique<slotnexus::backend::sherpa_onnx::SherpaKws>();
    if (!kws->load(kws_config)) {
      kws.reset();
      SetWakeError("KWS 模型加载失败: " + cfg.wake_model_dir);
      common::LogLine("session wake unavailable work_id=" + work_id +
                      " reason=" + WakeError());
      return false;
    }
#else
    SetWakeError("当前构建未启用语音唤醒（需硬件后端构建）");
    common::LogLine("session wake unavailable work_id=" + work_id +
                    " reason=" + WakeError());
    return false;
#endif

#ifdef SLOTNEXUS_HAS_ALSA
    auto source = std::make_unique<slotnexus::backend::alsa::AlsaAudioSource>(
        cfg.record_device, slotnexus::backend::kSampleRateHz);
    if (!source->open()) {
      SetWakeError("录音设备打开失败: " + cfg.record_device);
      common::LogLine("session wake unavailable work_id=" + work_id +
                      " reason=" + WakeError());
      return false;
    }
    if (source->actual_sample_rate() != slotnexus::backend::kSampleRateHz) {
      source->close();
      SetWakeError("录音采样率 " + std::to_string(source->actual_sample_rate()) +
                   " Hz 与 ASR 要求 " +
                   std::to_string(slotnexus::backend::kSampleRateHz) +
                   " Hz 不一致");
      common::LogLine("session wake unavailable work_id=" + work_id +
                      " reason=" + WakeError());
      return false;
    }
#else
    SetWakeError("当前构建未启用 ALSA 连续采集");
    common::LogLine("session wake unavailable work_id=" + work_id +
                    " reason=" + WakeError());
    return false;
#endif

    session::StreamingInput::Config stream_cfg;
    stream_cfg.frame_samples =
        static_cast<std::size_t>(slotnexus::backend::kFrameSamples);
    const int frame_ms = 20;
    stream_cfg.pre_roll_frames =
        std::max<std::size_t>(1, cfg.stream_pre_roll_ms / frame_ms);
    stream_cfg.min_speech_frames =
        std::max<std::size_t>(1, cfg.stream_min_speech_ms / frame_ms);
    stream_cfg.min_silence_frames =
        std::max<std::size_t>(1, cfg.stream_min_silence_ms / frame_ms);
    stream_cfg.speech_rms_threshold = cfg.stream_speech_rms_threshold;
#ifdef SLOTNEXUS_HAS_SHERTA_ONNX
    if (!cfg.vad_model.empty()) {
      vad = std::make_shared<slotnexus::backend::sherpa_onnx::SherpaVad>(
          cfg.vad_model, slotnexus::backend::kSampleRateHz);
      if (vad->ready()) {
        common::LogLine("session wake vad model=" + cfg.vad_model);
        std::shared_ptr<slotnexus::backend::sherpa_onnx::SherpaVad> shared_vad =
            vad;
        stream_cfg.speech_detector = [shared_vad](
                                         const std::vector<std::int16_t>& frame) {
          return shared_vad->is_speech(frame.data(), frame.size());
        };
      } else {
        vad.reset();
        common::LogLine("session wake vad unavailable model=" + cfg.vad_model);
      }
    }
#endif

    session::WakeGate::Config gate_cfg;
    gate_cfg.enabled = true;
    gate_cfg.wake_words = cfg.wake_words;
    gate_cfg.sleep_words = cfg.sleep_words;
    gate_cfg.follow_up_timeout = cfg.wake_follow_up_timeout;
    gate_cfg.max_session = cfg.wake_max_session;
    gate_cfg.max_turns = cfg.wake_max_turns;
    wake_gate.configure(gate_cfg);

    stream_input = std::make_unique<session::StreamingInput>(stream_cfg, *asr);
    stream_input->set_callbacks(
        [this, work_id](std::string text) {
          const session::WakeGate::Result result = wake_gate.process(text);
          if (result.answer) {
            EnqueueTurn(std::move(result.text));
          }
          if (result.slept || result.expired) {
            ResetKws();
            LogWakeState(work_id, "final");
          }
        },
        {},
        [this] { ResetKws(); },
        [this] { wake_gate.speech_started(); });

    input_stop.store(false);
    wake_available.store(true);
    SetWakeError({});
#ifdef SLOTNEXUS_HAS_ALSA
    capture_thread = std::thread(
        [this, source = std::move(source), work_id]() mutable {
          while (!input_stop.load()) {
            auto chunk = source->read(slotnexus::backend::kFrameSamples);
            if (chunk.empty()) {
              TickWake(work_id);
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
              continue;
            }
            FeedWakeFrame(chunk.data(), chunk.size(), work_id);
            TickWake(work_id);
          }
          source->close();
        });
#endif
    turn_thread = std::thread(&Session::TurnLoop, this, cfg, work_id);
    common::LogLine("session wake started work_id=" + work_id +
                    " device=" + cfg.record_device +
                    " wake_words=" + std::to_string(cfg.wake_words.size()) +
                    " max_turns=" + std::to_string(cfg.wake_max_turns));
    return true;
  }
};

// 后端工厂：按配置模式创建 asr/llm/tts 后端。
//  - embedded：本地 Fake（确定性，M1 基线）；
//  - net：远端节点代理（work_id 与节点任务一致；构造时同步 setup 节点，
//    失败抛异常 → 会话 setup 失败，客户端早失败早清楚）。
std::unique_ptr<slotnexus::backend::IAsrBackend> MakeAsrBackend(
    zmq::context_t& ctx, const SessionNodeConfig& cfg,
    const std::string& work_id) {
  if (cfg.backend == "net") {
    slotnexus::backend::net::NetBackendConfig c{
        cfg.asr_ep.rpc, cfg.asr_ep.events, cfg.asr_ep.sync, work_id,
        cfg.net_setup_timeout, cfg.net_rpc_timeout};
    c.asr_audio_uplink = cfg.asr_audio_uplink;
    c.asr_stream_endpoint = cfg.asr_stream_endpoint;
    if (!cfg.asr_stream_endpoint.empty()) {
      return std::make_unique<slotnexus::backend::net::NetAsrStreamBackend>(
          ctx, std::move(c));
    }
    return std::make_unique<slotnexus::backend::net::NetAsrBackend>(
        ctx, std::move(c));
  }
  return std::make_unique<slotnexus::backend::fake::FakeAsrBackend>();
}

std::unique_ptr<slotnexus::backend::ILlmBackend> MakeLlmBackend(
    zmq::context_t& ctx, const SessionNodeConfig& cfg,
    const std::string& work_id) {
  if (cfg.backend == "net") {
    return std::make_unique<slotnexus::backend::net::NetLlmBackend>(
        ctx, slotnexus::backend::net::NetBackendConfig{
                 cfg.llm_ep.rpc, cfg.llm_ep.events, cfg.llm_ep.sync, work_id,
                 cfg.net_setup_timeout, cfg.net_rpc_timeout});
  }
  return std::make_unique<slotnexus::backend::fake::FakeLlmBackend>();
}

std::unique_ptr<slotnexus::backend::ITtsBackend> MakeTtsBackend(
    zmq::context_t& ctx, const SessionNodeConfig& cfg,
    const std::string& work_id) {
  if (cfg.backend == "net") {
    return std::make_unique<slotnexus::backend::net::NetTtsBackend>(
        ctx, slotnexus::backend::net::NetBackendConfig{
                 cfg.tts_ep.rpc, cfg.tts_ep.events, cfg.tts_ep.sync, work_id,
                 cfg.net_setup_timeout, cfg.net_rpc_timeout});
  }
  return std::make_unique<slotnexus::backend::fake::FakeTtsBackend>();
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
      s->StopWakeInput();
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
        const auto setup_start = std::chrono::steady_clock::now();
        const auto setup_elapsed_ms = [setup_start] {
          return std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - setup_start)
              .count();
        };

        // 后端创建（net 模式含节点 setup RPC，模型加载发生在各节点
        // setup）；逐项记录耗时，失败 → 会话 setup 失败。
        // 错误码 4 = 后端不可用（节点未启动/连接超时）。
        try {
          s->asr = MakeAsrBackend(ctx_, config_, request.work_id());
          s->asr_setup_ms = setup_elapsed_ms();
          s->llm = MakeLlmBackend(ctx_, config_, request.work_id());
          s->llm_setup_ms = setup_elapsed_ms();
          s->tts = MakeTtsBackend(ctx_, config_, request.work_id());
          s->tts_setup_ms = setup_elapsed_ms();
        } catch (const std::exception& e) {
          log_err(request, std::string("backend_unavailable: ") + e.what());
          send_reply(build_error(request, 4,
                                 "后端不可用: " + std::string(e.what())));
          return;
        }

        session::SessionPipeline::SinkFactory sink_factory;
        if (config_.output_sink == "wav") {
          sink_factory = [](const std::string& path) {
            return std::make_unique<slotnexus::backend::fake::FakeAudioSink>(
                path);
          };
        } else if (config_.output_sink == "alsa") {
#ifdef SLOTNEXUS_HAS_ALSA
          const std::string device = config_.output_device;
          sink_factory = [device](const std::string& /*wav_path*/) {
            return std::make_unique<slotnexus::backend::alsa::AlsaAudioSink>(
                device, slotnexus::backend::kSampleRateHz);
          };
#else
          log_err(request, "alsa_sink_unavailable");
          send_reply(build_error(
              request, 3,
              "当前构建未启用 ALSA 输出（需硬件后端构建，--sink alsa）"));
          return;
#endif
        } else {
          log_err(request, "unknown_output_sink");
          send_reply(build_error(request, 3,
                                 "未知输出目标: " + config_.output_sink));
          return;
        }

        s->pipeline = std::make_unique<session::SessionPipeline>(
            PipelineConfig{config_.text_capacity, config_.pcm_capacity,
                           config_.text_chunk_max_bytes, config_.push_timeout,
                           config_.stage_delay, config_.output_dir,
                           config_.tts_min_duration, config_.output_sink},
            *router_, *s->asr, *s->llm, *s->tts, std::move(sink_factory));
        // 唤醒常驻采集必须在 pipeline 创建后才启动；失败不阻断 setup，
        // 手动 mode=stream 仍可用。
        s->StartWakeInput(config_, request.work_id());
        s->setup_ms = setup_elapsed_ms();
        common::LogLine(
            "session setup done request_id=" + request.request_id() +
            " work_id=" + request.work_id() + " sink=" + config_.output_sink +
            " setup_ms=" + std::to_string(s->setup_ms) +
            " asr_ms=" + std::to_string(s->asr_setup_ms) +
            " llm_ms=" + std::to_string(s->llm_setup_ms) +
            " tts_ms=" + std::to_string(s->tts_setup_ms));
        sessions_[request.work_id()] = s;
      }
      MessageEnvelope ack;
      ack.set_type(MessageType::kAck);
      ack.set_work_id(request.work_id());
      ack.set_request_id(request.request_id());
      ack.set_session_id(request.session_id());
      ack.set_payload({{"status", "ok"},
                       {"output_mode", config_.output_sink},
                       {"setup_ms", s->setup_ms},
                       {"asr_setup_ms", s->asr_setup_ms},
                       {"llm_setup_ms", s->llm_setup_ms},
                       {"tts_setup_ms", s->tts_setup_ms},
                       {"wake_enabled", config_.wake_enabled},
                       {"wake_available", s->wake_available.load()},
                       {"wake_state", s->WakeState()},
                       {"wake_error", s->WakeError()}});
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
      // 解析输入：{"mode": "text"|"wav"|"stream", ...}。stream 为无 KWS
      // 手动入口，在采集侧完成判停后把 final 文本送入文本路由。
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
      } else if (mode == "stream") {
        if (s->WakeInputActive()) {
          log_err(request, "wake_input_active");
          send_reply(build_error(
              request, 3,
              "常驻唤醒采集已启用，请直接使用唤醒词；不可重复打开录音设备"));
          return;
        }
#ifdef SLOTNEXUS_HAS_ALSA
        // 连续采集：VAD 判停后只取 final 文本交给文本路由。
        // stream_max_duration 只作无语音/不判停的兜底上限。
        slotnexus::backend::alsa::AlsaAudioSource mic(
            config_.record_device, slotnexus::backend::kSampleRateHz);
        if (!mic.open()) {
          log_err(request, "mic_open_failed");
          send_reply(build_error(request, 3,
                                 "录音设备打开失败: " + config_.record_device));
          return;
        }

        session::StreamingInput::Config stream_cfg;
        stream_cfg.frame_samples = static_cast<std::size_t>(
            slotnexus::backend::kFrameSamples);
        const int frame_ms = 20;
        stream_cfg.pre_roll_frames =
            std::max<std::size_t>(1, config_.stream_pre_roll_ms / frame_ms);
        stream_cfg.min_speech_frames =
            std::max<std::size_t>(1, config_.stream_min_speech_ms / frame_ms);
        stream_cfg.min_silence_frames =
            std::max<std::size_t>(1, config_.stream_min_silence_ms / frame_ms);
        stream_cfg.speech_rms_threshold = config_.stream_speech_rms_threshold;
#ifdef SLOTNEXUS_HAS_SHERTA_ONNX
        std::unique_ptr<slotnexus::backend::sherpa_onnx::SherpaVad> silero_vad;
        if (!config_.vad_model.empty()) {
          silero_vad =
              std::make_unique<slotnexus::backend::sherpa_onnx::SherpaVad>(
                  config_.vad_model, slotnexus::backend::kSampleRateHz);
          if (silero_vad->ready()) {
            common::LogLine("session vad model=" + config_.vad_model);
            stream_cfg.speech_detector =
                [&](const std::vector<std::int16_t>& frame) {
                  return silero_vad->is_speech(frame.data(), frame.size());
                };
          } else {
            silero_vad.reset();
            std::cerr << "[session] VAD 模型加载失败，退回能量阈值: "
                      << config_.vad_model << std::endl;
          }
        }
#endif

        std::string final_text;
        bool got_final = false;
        std::chrono::steady_clock::time_point endpoint_time{};
        std::chrono::steady_clock::time_point final_time{};
        session::StreamingInput stream(stream_cfg, *s->asr);
        stream.set_callbacks(
            [&](std::string text) {
              final_text = std::move(text);
              final_time = std::chrono::steady_clock::now();
              got_final = true;
            },
            {},
            [&] { endpoint_time = std::chrono::steady_clock::now(); });

        const auto capture_start = std::chrono::steady_clock::now();
        const auto capture_deadline = capture_start + config_.stream_max_duration;
        std::size_t empty_reads = 0;
        std::size_t retries = 0;
        std::size_t total_samples = 0;
        double sum_sq = 0.0;
        int16_t peak = 0;
        while (!got_final &&
               std::chrono::steady_clock::now() < capture_deadline) {
          auto chunk = mic.read(slotnexus::backend::kFrameSamples);
          if (chunk.empty()) {
            ++empty_reads;
            ++retries;
            continue;
          }
          total_samples += chunk.size();
          for (int16_t sample : chunk) {
            const double value = static_cast<double>(sample);
            sum_sq += value * value;
            const int magnitude = std::abs(static_cast<int>(sample));
            if (magnitude > peak) {
              peak = magnitude;
            }
          }
          stream.feed_audio(chunk.data(), chunk.size());
        }
        if (!got_final) {
          stream.flush();
        }
        mic.close();
        const auto capture_end = std::chrono::steady_clock::now();
        const auto capture_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                capture_end - capture_start)
                .count();
        const auto endpoint_to_final_ms =
            endpoint_time.time_since_epoch().count() == 0
                ? -1
                : std::chrono::duration_cast<std::chrono::milliseconds>(
                      final_time - endpoint_time)
                      .count();
        const double rms =
            total_samples == 0
                ? 0.0
                : std::sqrt(sum_sq / static_cast<double>(total_samples));
        common::LogLine(
            "session stream request_id=" + request.request_id() +
            " device=" + config_.record_device +
            " requested_max_ms=" +
            std::to_string(config_.stream_max_duration.count()) +
            " samples=" + std::to_string(total_samples) +
            " empty_reads=" + std::to_string(empty_reads) +
            " retries=" + std::to_string(retries) +
            " capture_ms=" + std::to_string(capture_ms) +
            " endpoint_to_final_ms=" + std::to_string(endpoint_to_final_ms) +
            " rms=" + std::to_string(rms) + " peak=" +
            std::to_string(peak) + " got_final=" +
            (got_final ? "1" : "0") + " asr_text=" + final_text);
        if (!got_final) {
          log_err(request, "stream_no_final");
          send_reply(build_error(
              request, 3,
              "连续采集未产生 ASR final（未检测到有效语音或未判停）"));
          return;
        }
        input.mode = PipelineInput::Mode::kText;
        input.text = final_text;
#else
        send_reply(build_error(
            request, 3,
            "当前构建未启用 ALSA 连续采集（需硬件后端构建，mode=stream）"));
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
      p["wake_enabled"] = s->wake_enabled.load();
      p["wake_available"] = s->wake_available.load();
      p["wake_state"] = s->WakeState();
      p["wake_turns"] = s->wake_turns.load();
      p["wake_error"] = s->WakeError();
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
      s->StopWakeInput();  // 常驻采集退出后再让在途推理失效
      s->pipeline->cancel();
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
  if (!result.wav_path.empty()) {
    p["wav_path"] = result.wav_path;
  }
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
  common::LogLine(
      "session done request_id=" + request_id + " work_id=" + work_id +
      " status=" + status + " route=" + result.route +
      " tokens=" + std::to_string(result.token_count) +
      " pcm=" + std::to_string(result.pcm_frames) +
      " sink=" + result.output_mode +
      " delivered=" + (result.audio_delivered ? "1" : "0") +
      " wav_done=" + (result.wav_complete ? "1" : "0") +
      " playback_done=" + (result.playback_complete ? "1" : "0") +
      " total_ms=" + std::to_string(result.total_ms) +
      " asr_final_ms=" + std::to_string(result.asr_final_ms) +
      " llm_first_token_ms=" + std::to_string(result.llm_first_token_ms) +
      " tts_first_pcm_ms=" + std::to_string(result.tts_first_pcm_ms) +
      " first_output_ms=" + std::to_string(result.first_output_ms) +
      (result.sink_error.empty() ? "" : " sink_error=" + result.sink_error) +
      (result.error.empty() ? "" : " error=" + result.error) +
      (result.wav_path.empty() ? "" : " wav=" + result.wav_path));
  reply.set_payload(std::move(p));
  reply.set_finish(true);
  try {
    push.send(zmq::buffer(identity), zmq::send_flags::sndmore);
    push.send(zmq::buffer(reply.to_json()), zmq::send_flags::none);
  } catch (const zmq::error_t&) {
    // close 后管道不可用：丢弃（进程正在退出）。
  }
}

}  // namespace slotnexus::app
