#include "slotnexus/session/session_pipeline.hpp"
// Author: Caden

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <utility>

#include "slotnexus/backend/backend_event.hpp"
#include "slotnexus/common/sentence_chunker.hpp"
#include "slotnexus/common/wav_reader.hpp"

namespace slotnexus::session {

namespace {

using backend::BackendEvent;
using common::BoundedQueue;
using common::QueueResult;
using SessionEvent = SessionStateMachine::Event;

// 文件内容哈希（FNV-1a，确定性文件名）。
std::string text_hash(const std::string& s) {
  std::uint32_t h = 2166136261u;
  for (unsigned char c : s) {
    h ^= c;
    h *= 16777619u;
  }
  char buf[9];
  std::snprintf(buf, sizeof(buf), "%08x", h);
  return buf;
}

// 工作线程出队等待间隔（空队列时轮询周期，保持可退出）。
constexpr std::chrono::milliseconds kPollInterval(10);

}  // namespace

SessionPipeline::SessionPipeline(PipelineConfig config, rag::Router& router,
                                 backend::IAsrBackend& asr,
                                 backend::ILlmBackend& llm,
                                 backend::ITtsBackend& tts,
                                 SinkFactory sink_factory)
    : config_(std::move(config)),
      router_(router),
      asr_(asr),
      llm_(llm),
      tts_(tts),
      sink_factory_(std::move(sink_factory)) {}

PipelineResult SessionPipeline::run(const PipelineInput& input,
                                    const std::string& request_id,
                                    std::chrono::milliseconds deadline) {
  // 单流：同一管线同一时刻只允许一个 run 在途。
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    PipelineResult busy;
    busy.error = "已有在途会话（单流）";
    return busy;
  }
  struct RunningGuard {
    std::atomic<bool>& flag;
    ~RunningGuard() { flag.store(false); }
  } running_guard{running_};

  cancelled_.store(false);
  state_machine_.reset();

  const std::uint64_t my_gen = generation_.fetch_add(1) + 1;
  active_generation_ = my_gen;
  active_request_id_ = request_id;

  PipelineResult result;
  result.generation = my_gen;
  result.output_mode = config_.output_mode;

  // deadline.count() <= 0 表示不限时；否则从 run 开始计时。
  const auto start_time = std::chrono::steady_clock::now();
  const auto timed_out = [deadline, start_time] {
    return deadline.count() > 0 &&
           std::chrono::steady_clock::now() >= start_time + deadline;
  };
  const auto elapsed_ms = [&start_time] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start_time)
        .count();
  };

  BoundedQueue<std::string> text_queue(config_.text_queue_capacity);
  BoundedQueue<std::vector<std::int16_t>> pcm_queue(
      config_.pcm_queue_capacity);

  // 输出 sink：WAV 模式生成确定文件路径，ALSA 模式路径为空（工厂按
  // 配置创建设备 sink）。打开失败立即失败并保留可观察错误。
  std::string output_path;
  if (config_.output_mode == "wav") {
    output_path = make_wav_path(request_id);
    result.wav_path = output_path;
  }
  auto sink = sink_factory_(output_path);
  if (!sink || !sink->open()) {
    result.sink_error = "音频输出打开失败";
    result.error = result.sink_error +
                   (output_path.empty() ? std::string()
                                        : ": " + output_path);
    result.total_ms = elapsed_ms();
    return result;
  }
  std::atomic<bool> sink_write_failed{false};

  // 事件双检查：旧世代/旧请求（取消后的晚到回调）一律丢弃。
  const auto is_active = [&] {
    return !cancelled_.load() && my_gen == active_generation_ &&
           request_id == active_request_id_;
  };

  // 文本入队：满队列等待 queue_push_timeout，仍满则丢弃并计数（明确行为）。
  auto push_text = [&](const std::string& sentence) {
    const auto r = text_queue.push_timeout(sentence, config_.queue_push_timeout);
    if (r == QueueResult::kFull) {
      ++result.dropped_sentences;  // 驱动线程独占，无需原子
    } else if (r == QueueResult::kOk && result.first_text_ms < 0) {
      result.first_text_ms = elapsed_ms();  // 驱动线程独占
    }
  };
  // 回答文本（L0/L1 直答或 LLM 输出）→ 分句 → 文本队列。
  auto feed_answer_sentences = [&](const std::string& text) {
    common::SentenceChunker chunker;
    for (const auto& s : chunker.feed(text)) {
      push_text(s);
    }
    for (const auto& s : chunker.flush()) {
      push_text(s);
    }
  };

  std::thread tts_thread;
  std::thread sink_thread;
  bool workers_spawned = false;

  try {
    // ---------- 1. 输入阶段：WAV / 麦克风 → ASR（Listening）----------
    // kWav / kMic 同一条样本链路：kWav 由管线读文件，kMic 直接用会话侧
    // 录制样本（AlsaAudioSource 在会话进程完成采集，管线不依赖声卡）。
    state_machine_.dispatch(SessionEvent::kAudioStart);
    std::string query;
    if (input.mode == PipelineInput::Mode::kWav ||
        input.mode == PipelineInput::Mode::kMic) {
      std::vector<std::int16_t> samples;
      if (input.mode == PipelineInput::Mode::kWav) {
        const auto wav = common::WavReader::read(input.wav_path);
        if (!wav.ok) {
          result.error = wav.error;
        } else if (wav.info.sample_rate != backend::kSampleRateHz ||
                   wav.info.channels != backend::kChannels ||
                   wav.info.bits != 16) {
          result.error = "固定输入须为 16kHz 单声道 16-bit WAV";
        } else if (wav.info.samples.empty()) {
          result.error = "WAV 无音频数据";
        } else {
          samples = wav.info.samples;
        }
      } else {
        samples = input.audio;
        if (samples.empty()) {
          result.error = "录音无音频数据";
        }
      }
      if (result.error.empty()) {
        asr_.set_event_callback([&](const BackendEvent& e) {
          if (!is_active()) {
            return;  // 取消后 ASR 的晚到事件：丢弃
          }
          if (e.kind == BackendEvent::Kind::kFinal) {
            query = e.text;
            result.asr_final_ms = elapsed_ms();
          }
        });
        constexpr std::size_t kFrame =
            static_cast<std::size_t>(backend::kFrameSamples);
        for (std::size_t off = 0; off < samples.size(); off += kFrame) {
          if (cancelled_.load() || timed_out()) {
            break;
          }
          const std::size_t end = std::min(off + kFrame, samples.size());
          const bool last = end == samples.size();
          std::vector<std::int16_t> frame(
              samples.begin() + static_cast<std::ptrdiff_t>(off),
              samples.begin() + static_cast<std::ptrdiff_t>(end));
          if (last) {
            // 固定输入的“喂入结束”：最后一帧交给 ASR 之前。连续采集
            // 接入后应由端点确认替换，不能把 ASR 往返时间算进输入时长。
            result.input_end_ms = elapsed_ms();
          }
          asr_.feed_audio(frame, last);
          if (config_.stage_delay.count() > 0) {
            std::this_thread::sleep_for(config_.stage_delay);
          }
        }
        if (result.input_end_ms < 0) {
          result.input_end_ms = elapsed_ms();
        }
        result.asr_text = query;
        if (query.find_first_not_of(" \t\r\n") == std::string::npos) {
          result.error = "ASR 未识别到文本";
        }
      }
      state_machine_.dispatch(SessionEvent::kAsrFinal);
    } else {
      // 文本模式：无音频输入，Listening 阶段为空。
      state_machine_.dispatch(SessionEvent::kAsrFinal);
      query = input.text;
      result.input_end_ms = elapsed_ms();
      result.asr_final_ms = result.input_end_ms;
      if (query.empty()) {
        result.error = "空文本请求";
      }
    }

    // ---------- 2. 路由阶段（Routing → 直答/Thinking）----------
    if (result.error.empty() && !cancelled_.load() && !timed_out()) {
      const auto decision = router_.route(query);
      result.decision = decision;
      result.route = rag::to_string(decision.level);

      if (decision.level == rag::RouteLevel::kL0 ||
          decision.level == rag::RouteLevel::kL1) {
        // L0/L1：绕过 LLM，直答文本进入 TTS。
        state_machine_.dispatch(SessionEvent::kRouteL0L1);
        result.final_text = decision.answer;
        feed_answer_sentences(decision.answer);
      } else {
        // L2/L3：调用 LLM，token 流经分句器进入文本队列。
        state_machine_.dispatch(SessionEvent::kRouteL2L3);
        result.llm_called = true;
        common::SentenceChunker chunker;
        llm_.set_event_callback([&](const BackendEvent& e) {
          if (!is_active()) {
            return;  // 取消后 LLM 的晚到 token：丢弃
          }
          if (e.kind == BackendEvent::Kind::kToken) {
            if (result.llm_first_token_ms < 0) {
              result.llm_first_token_ms = elapsed_ms();
            }
            ++result.token_count;
            if (!result.final_text.empty()) {
              result.final_text += " ";
            }
            result.final_text += e.text;
            for (const auto& s : chunker.feed(e.text)) {
              push_text(s);
            }
          }
          if (config_.stage_delay.count() > 0) {
            std::this_thread::sleep_for(config_.stage_delay);
          }
        });
        llm_.generate(decision.prompt);
        for (const auto& s : chunker.flush()) {
          push_text(s);
        }
        state_machine_.dispatch(SessionEvent::kLlmDone);
      }
    }

    // ---------- 3. 合成与写出阶段（Speaking，两个工作线程）----------
    if (result.error.empty() && !cancelled_.load() && !timed_out()) {
      // TTS 工作线程：句子 → 合成 → PCM 帧入有界队列。
      // 异常防护：网络后端（节点不可达/超时）可能抛异常，线程内未捕获
      // 会 terminate 整个进程——捕获后记录错误，主线程收尾据此判失败。
      tts_thread = std::thread([&] {
        try {
          std::string sentence;
          for (;;) {
            const auto r = text_queue.pop_timeout(sentence, kPollInterval);
            if (r == QueueResult::kClosed) {
              return;  // 生产结束且已排空
            }
            if (r != QueueResult::kOk) {
              continue;  // 空窗口，继续等待
            }
            if (!is_active()) {
              continue;  // 取消后滞留句子：丢弃
            }
            tts_.set_event_callback([&](const BackendEvent& e) {
              if (!is_active()) {
                return;  // 取消后 TTS 的晚到 PCM：丢弃
              }
              if (e.kind == BackendEvent::Kind::kPcm) {
                if (result.tts_first_pcm_ms < 0) {
                  result.tts_first_pcm_ms = elapsed_ms();
                }
                const auto pr =
                    pcm_queue.push_timeout(e.pcm, config_.queue_push_timeout);
                if (pr == QueueResult::kFull) {
                  ++result.dropped_pcm_frames;  // 满队列超时丢弃（明确行为）
                }
              }
            });
            tts_.synthesize(sentence);
            if (config_.stage_delay.count() > 0) {
              std::this_thread::sleep_for(config_.stage_delay);
            }
          }
        } catch (const std::exception& e) {
          result.error = std::string("TTS 阶段异常: ") + e.what();
        }
      });
      // 写出工作线程：PCM 帧 → sink（取消后滞留帧不写出）。
      sink_thread = std::thread([&] {
        std::vector<std::int16_t> frame;
        for (;;) {
          const auto r = pcm_queue.pop_timeout(frame, kPollInterval);
          if (r == QueueResult::kClosed) {
            return;
          }
          if (r != QueueResult::kOk) {
            continue;
          }
          if (!is_active()) {
            continue;  // 取消后滞留帧：不写入输出
          }
          if (sink->write_pcm(frame)) {
            if (result.first_output_ms < 0) {
              result.first_output_ms = elapsed_ms();
            }
            ++result.pcm_frames;
          } else {
            sink_write_failed.store(true);
            result.sink_error = "音频输出写入失败";
            return;  // sink 已错误：停止继续提交，主线程按失败收尾
          }
        }
      });
      workers_spawned = true;
    }

  } catch (const std::exception& e) {
    result.error = std::string("管线异常: ") + e.what();
  }

  // ---------- 收尾（全部路径汇合）：关闭队列 → 排空工作线程 → 关输出 ----------
  text_queue.close();
  if (tts_thread.joinable()) {
    tts_thread.join();
  }
  pcm_queue.close();
  if (sink_thread.joinable()) {
    sink_thread.join();
  }

  // 最小合成时长：输出不足时补静音帧（成功路径；取消/失败不补齐）。
  if (!cancelled_.load() && result.error.empty() && !timed_out() &&
      config_.tts_min_duration.count() > 0) {
    const std::size_t min_frames = static_cast<std::size_t>(
        config_.tts_min_duration.count() * backend::kSampleRateHz /
        1000 / backend::kFrameSamples);
    while (result.pcm_frames < min_frames) {
      std::vector<std::int16_t> silence(
          static_cast<std::size_t>(backend::kFrameSamples), 0);
      if (sink->write_pcm(silence)) {
        if (result.first_output_ms < 0) {
          result.first_output_ms = elapsed_ms();
        }
        ++result.pcm_frames;
      } else {
        sink_write_failed.store(true);
        result.sink_error = "音频输出补齐静音失败";
        break;
      }
    }
  }

  const bool sink_ok = sink->close();
  result.output_complete_ms = elapsed_ms();
  // “已交付”= 本次提交的 PCM 均被 sink 接受且 close 成功；它不等于
  // WAV 写完，更不等于扬声器真实播完。WAV/ALSA 的完成语义在下方按
  // output_mode 分列，避免把文件关闭误报成声学播放完成。
  result.audio_delivered =
      !sink_write_failed.load() && result.pcm_frames > 0 && sink_ok;
  if (sink_write_failed.load() && result.error.empty()) {
    result.error = result.sink_error.empty() ? "音频输出写入失败"
                                             : result.sink_error;
  }
  if (cancelled_.load()) {
    result.cancelled = true;
  }
  if (result.error.empty() && !result.cancelled && !timed_out()) {
    if (sink_ok) {
      state_machine_.dispatch(SessionEvent::kTtsDone);
      result.ok = true;
    } else {
      result.sink_error = "音频输出关闭失败";
      result.error = result.sink_error;
    }
  } else if (!sink_ok && result.sink_error.empty()) {
    result.sink_error = "音频输出关闭失败";
  }

  // 取消/超时/失败：状态机回到 Idle（Cancelling → cancel_complete）。
  if (!result.ok) {
    if (result.error.empty()) {
      result.error = timed_out() ? "管线超时" : "管线未完成";
    }
    state_machine_.dispatch(SessionEvent::kCancel);          // → Cancelling
    state_machine_.dispatch(SessionEvent::kCancelComplete);  // → Idle
  }

  if (result.ok) {
    result.wav_complete = config_.output_mode == "wav";
    result.playback_complete = config_.output_mode == "alsa";
  }

  // 统计与证据（工作线程已 join，无数据竞争）。
  result.text_queue_peak = text_queue.peak();
  result.pcm_queue_peak = pcm_queue.peak();
  result.transitions = state_machine_.trace();
  result.total_ms = elapsed_ms();
  return result;
}

void SessionPipeline::cancel() {
  // 递增世代：后续（含在途）回调全部失去活动性；新请求获得新世代。
  generation_.fetch_add(1);
  cancelled_.store(true);
  asr_.cancel();
  llm_.cancel();
  tts_.cancel();
  state_machine_.dispatch(SessionEvent::kCancel);
}

const char* SessionPipeline::state_name() const {
  return state_machine_.state_name();
}

std::vector<std::string> SessionPipeline::state_trace() const {
  return state_machine_.trace();
}

std::string SessionPipeline::make_wav_path(
    const std::string& request_id) const {
  std::error_code ec;
  std::filesystem::create_directories(config_.output_dir, ec);
  return config_.output_dir + "/session_" + text_hash(request_id) + ".wav";
}

}  // namespace slotnexus::session
