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
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "slotnexus/backend/fake/fake_backends.hpp"
#include "slotnexus/backend/i_asr_backend.hpp"
#include "slotnexus/backend/i_llm_backend.hpp"
#include "slotnexus/backend/i_tts_backend.hpp"
#ifdef SLOTNEXUS_HAS_ALSA
#include "slotnexus/backend/alsa/alsa_audio_sink.hpp"
#include "slotnexus/backend/alsa/alsa_audio_source.hpp"
#endif
#ifdef SLOTNEXUS_HAS_SHERTA_ONNX
#include "slotnexus/backend/sherpa_onnx/sherpa_vad.hpp"
#endif
#include "slotnexus/backend/net/net_asr_stream_backend.hpp"
#include "slotnexus/backend/net/net_llm_backend.hpp"
#include "slotnexus/backend/net/net_tts_backend.hpp"
#include "slotnexus/common/log.hpp"
#include "slotnexus/protocol/message_envelope.hpp"
#include "slotnexus/rag/knowledge_store.hpp"
#include "slotnexus/session/streaming_input.hpp"
#include "slotnexus/session/continuous_gate.hpp"

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
          {"dropped_sentences", 0},
          {"dropped_pcm_frames", 0},
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


// 连续采集参数：常驻连续会话与手动 mode=stream 共用同一套 20 ms 帧配置。
session::StreamingInput::Config MakeStreamConfig(const SessionNodeConfig& cfg) {
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
  if (cfg.continuous_pause_resume.count() > 0) {
    const std::size_t resume_frames = static_cast<std::size_t>(
        (cfg.continuous_pause_resume.count() + frame_ms - 1) / frame_ms);
    stream_cfg.resume_frames = std::max<std::size_t>(1, resume_frames);
  }
  stream_cfg.speech_rms_threshold = cfg.stream_speech_rms_threshold;
  return stream_cfg;
}

#ifdef SLOTNEXUS_HAS_SHERTA_ONNX
// 按配置加载 Silero VAD；失败或未配置时返回空，由 StreamingInput 退回能量阈值。
std::shared_ptr<slotnexus::backend::sherpa_onnx::SherpaVad> MakeVad(
    const std::string& model_path) {
  if (model_path.empty()) {
    return {};
  }
  auto vad = std::make_shared<slotnexus::backend::sherpa_onnx::SherpaVad>(
      model_path, slotnexus::backend::kSampleRateHz);
  if (vad->ready()) {
    common::LogLine("session vad model=" + model_path);
    return vad;
  }
  common::LogLine("session vad unavailable model=" + model_path);
  return {};
}
#endif

}  // namespace

// 一个会话实例：后端归本实例所有（embedded=Fake / net=节点代理，见
// MakeAsrBackend 等），管线只依赖接口引用。
struct SessionNode::Session {
  ~Session() { StopContinuousInput(); }

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

  // 连续交互常驻输入：采集线程只做 ALSA 帧流、VAD/ASR final 和收尾；
  // SessionPipeline 在单轮处理线程上串行执行。
  std::atomic<bool> continuous_enabled{false};
  mutable std::mutex continuous_meta_mutex;
  std::string continuous_error;
  std::unique_ptr<session::StreamingInput> stream_input;
  session::ContinuousGate continuous_gate;
  std::atomic<bool> input_stop{true};
  std::thread capture_thread;
  std::thread turn_thread;
  std::mutex turn_mutex;
  std::condition_variable turn_cv;

  // 一次待执行的连续轮次。request_id 保持逻辑轮次稳定，run_id 用于识别
  // 被续说取消的旧预推理；cancel_flag 覆盖“取消早于 run 启动”的窗口。
  struct PendingTurn {
    std::string text;
    std::string request_id;
    std::chrono::milliseconds commit_delay{0};
    std::uint64_t run_id = 0;
    std::shared_ptr<std::atomic<bool>> cancel_flag;
  };

  // 当前预推理/观察窗状态。text 是该逻辑轮次的最新合并文本。
  struct Speculation {
    bool active = false;
    bool merge_pending = false;
    bool resume_candidate = false;
    std::string text;
    std::string request_id;
    std::uint64_t run_id = 0;
    std::shared_ptr<std::atomic<bool>> cancel_flag;
    std::chrono::steady_clock::time_point deadline{};
  };

  std::deque<PendingTurn> pending_turns;
  std::mutex speculation_mutex_;
  Speculation speculation_;
  std::uint64_t next_run_id_ = 0;
  int turn_sequence_ = 0;
  std::chrono::milliseconds pause_observation_{500};

  std::string ContinuousError() const {
    std::lock_guard<std::mutex> lock(continuous_meta_mutex);
    return continuous_error;
  }

  void SetContinuousError(std::string error) {
    std::lock_guard<std::mutex> lock(continuous_meta_mutex);
    continuous_error = std::move(error);
  }

  std::string ContinuousState() const {
    if (!continuous_enabled.load()) {
      return "disabled";
    }
    if (stream_input == nullptr) {
      return "sleeping";
    }
    return continuous_gate.state_name();
  }

  bool ContinuousInputActive() const {
    return stream_input != nullptr && !input_stop.load();
  }

  void EnqueueTurn(PendingTurn turn) {
    if (turn.text.empty() || input_stop.load()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(turn_mutex);
      pending_turns.push_back(std::move(turn));
    }
    turn_cv.notify_one();
  }

  void OnContinuousFinal(std::string text, const std::string& work_id) {
    if (input_stop.load() || text.empty()) {
      return;
    }

    std::string gate_text;
    bool continuation = false;
    bool cancel_previous = false;
    bool start_new_turn = false;
    {
      std::lock_guard<std::mutex> lock(speculation_mutex_);
      if (speculation_.merge_pending) {
        continuation = true;
        gate_text = speculation_.text.empty()
                        ? std::move(text)
                        : speculation_.text + "，" + text;
        speculation_.merge_pending = false;
      } else {
        gate_text = std::move(text);
        if (speculation_.active) {
          const bool within_window =
              std::chrono::steady_clock::now() < speculation_.deadline;
          if (within_window) {
            // 观察窗内没有达到续说阈值、却先来了新的 final：按
            // latest-only 撤销旧预推理，避免旧回答和新 final 同时成立。
            cancel_previous = true;
            if (speculation_.cancel_flag) {
              speculation_.cancel_flag->store(true);
            }
            speculation_.active = false;
          } else {
            // 观察窗已经结束；旧 run 可能仍在写出，新 final 应按下一轮
            // 接纳，先把 ContinuousGate 从 Processing 拉回 Listening。
            start_new_turn = true;
          }
        }
      }
    }
    if (cancel_previous) {
      // 起音时若仍在观察窗内，OnContinuousSpeechStarted 没有调用
      // speech_started；这里需要为紧接着的新 final 打开 Listening。
      continuous_gate.speech_started();
      common::LogLine("session continuous speculative cancel work_id=" +
                      work_id + " reason=latest-final");
      if (pipeline) {
        pipeline->cancel_generation();
      }
    } else if (start_new_turn) {
      continuous_gate.speech_started();
    }

    const auto result = continuous_gate.process(gate_text, continuation);
    if (!result.answer) {
      if (result.slept || result.expired) {
        LogContinuousState(work_id, "final");
      }
      return;
    }

    PendingTurn turn;
    turn.text = result.text;
    turn.commit_delay = pause_observation_;
    turn.cancel_flag = std::make_shared<std::atomic<bool>>(false);
    {
      std::lock_guard<std::mutex> lock(speculation_mutex_);
      turn.request_id = continuation && !speculation_.request_id.empty()
                            ? speculation_.request_id
                            : "continuous-" + std::to_string(++turn_sequence_);
      turn.run_id = ++next_run_id_;
      speculation_.active = true;
      speculation_.merge_pending = false;
      speculation_.resume_candidate = false;
      speculation_.text = turn.text;
      speculation_.request_id = turn.request_id;
      speculation_.run_id = turn.run_id;
      speculation_.cancel_flag = turn.cancel_flag;
      speculation_.deadline =
          std::chrono::steady_clock::now() + pause_observation_;
    }
    common::LogLine(
        "session continuous speculative request_id=" + turn.request_id +
        " work_id=" + work_id + " run_id=" + std::to_string(turn.run_id) +
        " observation_ms=" + std::to_string(turn.commit_delay.count()) +
        " merged=" + (continuation ? std::string("1") : std::string("0")) +
        " text=" + turn.text);
    EnqueueTurn(std::move(turn));
  }

  void OnContinuousSpeechStarted(const std::string& work_id) {
    bool candidate = false;
    {
      std::lock_guard<std::mutex> lock(speculation_mutex_);
      if (speculation_.active &&
          std::chrono::steady_clock::now() < speculation_.deadline) {
        speculation_.resume_candidate = true;
        candidate = true;
      }
    }
    // 观察窗内的起音先不改变轮次状态；若 96 ms 内中断，仍按一轮提交，
    // 若达到阈值由 resume 回调显式打开 Listening 并撤销旧预推理。
    if (!candidate) {
      continuous_gate.speech_started();
    }
    common::LogLine("session continuous speech_started work_id=" + work_id +
                    " resume_candidate=" +
                    (candidate ? std::string("1") : std::string("0")));
  }

  void OnContinuousResumeDetected(const std::string& work_id) {
    std::shared_ptr<std::atomic<bool>> cancel_flag;
    bool cancel = false;
    {
      std::lock_guard<std::mutex> lock(speculation_mutex_);
      if (speculation_.active && speculation_.resume_candidate) {
        speculation_.merge_pending = true;
        speculation_.active = false;
        speculation_.resume_candidate = false;
        cancel_flag = speculation_.cancel_flag;
        if (cancel_flag) {
          cancel_flag->store(true);
        }
        cancel = true;
      }
    }
    if (!cancel) {
      return;
    }
    // 续说 final 会走 continuation 路径；先把 ContinuousGate 从
    // Processing 拉回 Listening，保证合并 final 能被接纳。
    continuous_gate.speech_started();
    common::LogLine("session continuous pause resume work_id=" + work_id +
                    " action=cancel-speculation");
    if (pipeline) {
      pipeline->cancel_generation();
    }
  }

  bool OnContinuousRunFinished(std::uint64_t run_id) {
    std::lock_guard<std::mutex> lock(speculation_mutex_);
    // 旧 run 被续说/最新 final 取代后，新预推理已占用 gate 状态；
    // 它不能再调用 finish_turn()，否则会把新轮的 Processing 误切到 FollowUp。
    if (speculation_.run_id != run_id || speculation_.merge_pending) {
      return false;
    }
    speculation_.active = false;
    speculation_.resume_candidate = false;
    return true;
  }

  void LogContinuousState(const std::string& work_id, const char* event) {
    common::LogLine("session continuous state=" +
                    std::string(continuous_gate.state_name()) + " work_id=" +
                    work_id + " event=" + event);
  }

  void FeedContinuousFrame(const std::int16_t* pcm, std::size_t count) {
    stream_input->feed_audio(pcm, count);
  }

  void TickContinuous(const std::string& work_id) {
    if (continuous_gate.tick()) {
      LogContinuousState(work_id, "tick");
    }
  }

  void ResumeContinuousInput(const std::string& work_id) {
    if (continuous_gate.state() == session::ContinuousGate::State::kSleeping) {
      continuous_gate.start();
      LogContinuousState(work_id, "start");
    }
  }

  void StopContinuousInput() {
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
  }

  void TurnLoop(SessionNodeConfig cfg, std::string work_id) {
    while (!input_stop.load()) {
      PendingTurn turn;
      {
        std::unique_lock<std::mutex> lock(turn_mutex);
        turn_cv.wait(lock, [this] {
          return input_stop.load() || !pending_turns.empty();
        });
        if (input_stop.load() && pending_turns.empty()) {
          break;
        }
        turn = std::move(pending_turns.front());
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
      if (!acquired || input_stop.load()) {
        busy.store(false);
        break;
      }
      if (turn.cancel_flag && turn.cancel_flag->load()) {
        common::LogLine("session continuous turn skip stale request_id=" +
                        turn.request_id + " work_id=" + work_id +
                        " run_id=" + std::to_string(turn.run_id));
        busy.store(false);
        continue;
      }

      session::PipelineInput input;
      input.mode = session::PipelineInput::Mode::kText;
      input.text = turn.text;
      input.commit_delay = turn.commit_delay;
      input.cancel_requested = turn.cancel_flag.get();
      common::LogLine(
          "session continuous turn request_id=" + turn.request_id +
          " work_id=" + work_id + " run_id=" + std::to_string(turn.run_id) +
          " observation_ms=" + std::to_string(turn.commit_delay.count()) +
          " text=" + turn.text);
      const session::PipelineResult result =
          pipeline->run(input, turn.request_id, cfg.max_run);
      {
        std::lock_guard<std::mutex> lock(last_mutex);
        last_result = result;
        last_request_id = turn.request_id;
      }
      busy.store(false);
      const bool finish_turn = OnContinuousRunFinished(turn.run_id);
      if (finish_turn) {
        continuous_gate.finish_turn();
      }
      std::string answer = result.final_text;
      for (char& ch : answer) {
        if (ch == '\r' || ch == '\n') {
          ch = ' ';
        }
      }
      common::LogLine(
          "session continuous done request_id=" + turn.request_id +
          " work_id=" + work_id +
          " run_id=" + std::to_string(turn.run_id) +
          " status=" + (result.ok ? std::string("ok")
                                  : (result.cancelled ? std::string("cancelled")
                                                      : std::string("error"))) +
          " route=" + result.route +
          " asr_text=" + result.asr_text +
          " total_ms=" + std::to_string(result.total_ms) +
          (result.error.empty() ? "" : " error=" + result.error) +
          (answer.empty() ? "" : " final_text=" + answer));
    }
  }

  bool StartContinuousInput(const SessionNodeConfig& cfg,
                            const std::string& work_id) {
    if (!cfg.continuous_enabled) {
      SetContinuousError("未启用连续交互");
      common::LogLine("session continuous unavailable work_id=" + work_id +
                      " reason=" + ContinuousError());
      return false;
    }
    if (ContinuousInputActive()) {
      ResumeContinuousInput(work_id);
      return true;
    }

#ifdef SLOTNEXUS_HAS_ALSA
    auto source = std::make_unique<slotnexus::backend::alsa::AlsaAudioSource>(
        cfg.record_device, slotnexus::backend::kSampleRateHz);
    if (!source->open()) {
      SetContinuousError("录音设备打开失败: " + cfg.record_device);
      common::LogLine("session continuous unavailable work_id=" + work_id +
                      " reason=" + ContinuousError());
      return false;
    }
    if (source->actual_sample_rate() != slotnexus::backend::kSampleRateHz) {
      const int actual_rate = source->actual_sample_rate();
      source->close();
      SetContinuousError("录音采样率 " + std::to_string(actual_rate) +
                         " Hz 与 ASR 要求 " +
                         std::to_string(slotnexus::backend::kSampleRateHz) +
                         " Hz 不一致");
      common::LogLine("session continuous unavailable work_id=" + work_id +
                      " reason=" + ContinuousError());
      return false;
    }
#else
    SetContinuousError("当前构建未启用 ALSA 连续采集");
    common::LogLine("session continuous unavailable work_id=" + work_id +
                    " reason=" + ContinuousError());
    return false;
#endif

    session::StreamingInput::Config stream_cfg = MakeStreamConfig(cfg);
#ifdef SLOTNEXUS_HAS_SHERTA_ONNX
    if (auto vad = MakeVad(cfg.vad_model)) {
      stream_cfg.speech_detector = [vad](
                                       const std::vector<std::int16_t>& frame) {
        return vad->is_speech(frame.data(), frame.size());
      };
      stream_cfg.speech_detector_reset = [vad] { vad->reset(); };
    }
#endif

    session::ContinuousGate::Config gate_cfg;
    gate_cfg.enabled = true;
    gate_cfg.sleep_words = cfg.sleep_words;
    gate_cfg.follow_up_timeout = cfg.continuous_follow_up_timeout;
    gate_cfg.max_session = cfg.continuous_max_session;
    gate_cfg.max_turns = cfg.continuous_max_turns;
    continuous_gate.configure(gate_cfg);
    continuous_gate.start();
    pause_observation_ = cfg.continuous_pause_observation;

    stream_input = std::make_unique<session::StreamingInput>(stream_cfg, *asr);
    stream_input->set_callbacks(
        [this, work_id](std::string text) {
          OnContinuousFinal(std::move(text), work_id);
        },
        [this, work_id] {
          common::LogLine("session continuous endpoint work_id=" + work_id);
        },
        [this, work_id] {
          OnContinuousSpeechStarted(work_id);
        },
        [this, work_id] {
          OnContinuousResumeDetected(work_id);
        });

    input_stop.store(false);
    SetContinuousError({});
#ifdef SLOTNEXUS_HAS_ALSA
    capture_thread = std::thread(
        [this, source = std::move(source), work_id]() mutable {
          while (!input_stop.load()) {
            auto chunk = source->read(slotnexus::backend::kFrameSamples);
            if (chunk.empty()) {
              TickContinuous(work_id);
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
              continue;
            }
            FeedContinuousFrame(chunk.data(), chunk.size());
            TickContinuous(work_id);
          }
          source->close();
        });
#endif
    turn_thread = std::thread(&Session::TurnLoop, this, cfg, work_id);
    common::LogLine(
        "session continuous started work_id=" + work_id +
        " device=" + cfg.record_device +
        " max_turns=" + std::to_string(cfg.continuous_max_turns) +
        " pause_observation_ms=" +
        std::to_string(cfg.continuous_pause_observation.count()) +
        " pause_resume_ms=" +
        std::to_string(cfg.continuous_pause_resume.count()));
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
    if (cfg.asr_stream_endpoint.empty()) {
      throw std::invalid_argument("net 模式需要 --asr-stream-endpoint");
    }
    slotnexus::backend::net::NetBackendConfig c{
        cfg.asr_ep.rpc, cfg.asr_ep.events, cfg.asr_ep.sync, work_id,
        cfg.net_setup_timeout, cfg.net_rpc_timeout};
    c.asr_stream_endpoint = cfg.asr_stream_endpoint;
    return std::make_unique<slotnexus::backend::net::NetAsrStreamBackend>(
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
      s->StopContinuousInput();
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
        // 连续交互由客户端显式 mode=continuous 启动；setup 不打开录音设备。
        s->continuous_enabled.store(config_.continuous_enabled);
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
                       {"continuous_enabled", config_.continuous_enabled},
                       {"continuous_active", s->ContinuousInputActive()},
                       {"continuous_state", s->ContinuousState()},
                       {"continuous_error", s->ContinuousError()}});
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
      const auto& payload = request.payload();
      const std::string mode = payload.value("mode", "text");
      if (mode == "continuous") {
        if (!config_.continuous_enabled) {
          log_err(request, "continuous_disabled");
          send_reply(build_error(request, 3, "连续交互未启用"));
          return;
        }
        if (!s->ContinuousInputActive()) {
          if (!s->StartContinuousInput(config_, request.work_id())) {
            log_err(request, "continuous_start_failed");
            send_reply(build_error(
                request, 3,
                "连续交互启动失败: " + s->ContinuousError()));
            return;
          }
        } else {
          s->ResumeContinuousInput(request.work_id());
        }
        MessageEnvelope ack;
        ack.set_type(MessageType::kAck);
        ack.set_work_id(request.work_id());
        ack.set_request_id(request.request_id());
        ack.set_session_id(request.session_id());
        ack.set_payload({{"status", "ok"},
                         {"continuous_enabled", true},
                         {"continuous_active", s->ContinuousInputActive()},
                         {"continuous_state", s->ContinuousState()},
                         {"continuous_error", s->ContinuousError()}});
        ack.set_finish(true);
        send_reply(ack);
        return;
      }

      bool expected = false;
      if (!s->busy.compare_exchange_strong(expected, true)) {
        log_err(request, "busy");
        send_reply(build_error(request, 3, "会话忙碌（单流）"));  // kBusy
        return;
      }
      // 解析输入：{"mode": "text"|"wav"|"stream"|"continuous", ...}。
      // stream 为手动单轮入口，continuous 显式启动常驻连续会话。
      PipelineInput input;
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
        if (s->ContinuousInputActive()) {
          log_err(request, "continuous_input_active");
          s->busy.store(false);
          send_reply(build_error(
              request, 3,
              "常驻连续交互已启动；不可重复打开录音设备"));
          return;
        }
#ifdef SLOTNEXUS_HAS_ALSA
        // 连续采集：VAD 判停后只取 final 文本交给文本路由。
        // stream_max_duration 只作无语音/不判停的兜底上限。
        slotnexus::backend::alsa::AlsaAudioSource mic(
            config_.record_device, slotnexus::backend::kSampleRateHz);
        common::LogLine("session stream open request_id=" +
                        request.request_id() +
                        " device=" + config_.record_device);
        if (!mic.open()) {
          log_err(request, "mic_open_failed");
          s->busy.store(false);
          send_reply(build_error(request, 3,
                                 "录音设备打开失败: " + config_.record_device));
          return;
        }

        session::StreamingInput::Config stream_cfg = MakeStreamConfig(config_);
#ifdef SLOTNEXUS_HAS_SHERTA_ONNX
        if (auto vad = MakeVad(config_.vad_model)) {
          stream_cfg.speech_detector = [vad](
                                           const std::vector<std::int16_t>& frame) {
            return vad->is_speech(frame.data(), frame.size());
          };
          stream_cfg.speech_detector_reset = [vad] { vad->reset(); };
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
            "session stream close request_id=" + request.request_id() +
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
          s->busy.store(false);
          send_reply(build_error(
              request, 3,
              "连续采集未产生 ASR final（未检测到有效语音或未判停）"));
          return;
        }
        input.mode = PipelineInput::Mode::kText;
        input.text = final_text;
#else
        s->busy.store(false);
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
      p["continuous_enabled"] = s->continuous_enabled.load();
      p["continuous_active"] = s->ContinuousInputActive();
      p["continuous_state"] = s->ContinuousState();
      p["continuous_turns"] = s->continuous_gate.turns();
      p["continuous_error"] = s->ContinuousError();
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
      s->StopContinuousInput();  // 常驻采集退出后再让在途推理失效
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
