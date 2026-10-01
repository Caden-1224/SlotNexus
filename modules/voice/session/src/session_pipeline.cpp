#include "slotnexus/session/session_pipeline.hpp"
// Author: Caden

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
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

const char* SessionPipeline::StateName(State state) {
  switch (state) {
    case State::kIdle:       return "idle";
    case State::kListening:  return "listening";
    case State::kRouting:    return "routing";
    case State::kThinking:   return "thinking";
    case State::kSpeaking:   return "speaking";
    case State::kCancelling: return "cancelling";
  }
  return "unknown";
}

void SessionPipeline::reset_state() {
  state_.store(State::kIdle);
}


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
  reset_state();

  const std::uint64_t my_gen = generation_.fetch_add(1) + 1;
  active_generation_ = my_gen;
  active_request_id_ = request_id;

  const auto pre_cancelled = [&] {
    return input.cancel_requested != nullptr && input.cancel_requested->load();
  };
  if (pre_cancelled()) {
    cancelled_.store(true);
  }

  PipelineResult result;
  result.generation = my_gen;
  result.output_mode = config_.output_mode;
  if (pre_cancelled()) {
    result.cancelled = true;
    result.error = "预推理已在启动前取消";
    result.total_ms = 0;
    state_.store(State::kCancelling);
    state_.store(State::kIdle);
    return result;
  }

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
  std::atomic<bool> tts_failed{false};
  std::exception_ptr tts_exception;

  // 事件双检查：旧世代/旧请求（取消后的晚到回调）一律丢弃。
  const auto is_active = [&] {
    return !cancelled_.load() && my_gen == active_generation_ &&
           request_id == active_request_id_;
  };

  // 首帧 PCM 提交前等待观察窗；窗口结束或取消后返回。等待期间 LLM/TTS
  // 继续预推理，PCM 由有界队列暂存。
  const auto commit_deadline = start_time + input.commit_delay;
  const auto wait_for_commit = [&]() -> bool {
    if (input.commit_delay.count() <= 0) {
      return true;
    }
    std::unique_lock<std::mutex> lock(commit_mutex_);
    commit_cv_.wait_until(lock, commit_deadline, [&] { return !is_active(); });
    return is_active();
  };

  // 文本入队：满队列按 queue_push_timeout 重试，不丢弃。取消/超时或下游
  // 工作线程退出后返回 false；调用方据此停止继续分句。
  auto push_text = [&](const std::string& sentence) -> bool {
    while (is_active() && !timed_out()) {
      if (sink_write_failed.load()) {
        if (result.error.empty()) {
          result.error = "音频输出写入失败";
        }
        return false;
      }
      if (tts_failed.load()) {
        if (result.error.empty()) {
          result.error = "TTS 工作线程已停止";
        }
        return false;
      }
      const auto r =
          text_queue.push_timeout(sentence, config_.queue_push_timeout);
      if (r == QueueResult::kOk) {
        if (result.first_text_ms < 0) {
          result.first_text_ms = elapsed_ms();
        }
        return true;
      }
      if (r == QueueResult::kClosed) {
        if (result.error.empty()) {
          result.error = "文本队列已关闭";
        }
        return false;
      }
      // kFull：继续重试，由下游消费或取消/超时解除背压。
    }
    return false;
  };
  // 回答文本（L0/L1 直答或 LLM 输出）→ 分句 → 文本队列。
  auto feed_answer_sentences = [&](const std::string& text) -> bool {
    common::SentenceChunker chunker(config_.text_chunk_max_bytes);
    for (const auto& s : chunker.feed(text)) {
      if (!push_text(s)) {
        return false;
      }
    }
    for (const auto& s : chunker.flush()) {
      if (!push_text(s)) {
        return false;
      }
    }
    return true;
  };

  // PCM 入队：满队列重试；sink 失败、取消或超时后返回 false。
  auto push_pcm = [&](const std::vector<std::int16_t>& pcm) -> bool {
    while (is_active() && !timed_out() && !sink_write_failed.load()) {
      const auto r = pcm_queue.push_timeout(pcm, config_.queue_push_timeout);
      if (r == QueueResult::kOk) {
        return true;
      }
      if (r == QueueResult::kClosed) {
        return false;
      }
      // kFull：继续重试，由 sink 写出或取消/超时解除背压。
    }
    return false;
  };

  std::thread tts_thread;
  std::thread sink_thread;

  try {
    // TTS 工作线程：首个可播片段入队后立即消费，不等 LLM 整段生成结束。
    // 异常防护：网络后端可能抛异常；线程内捕获后由主线程收尾统一判失败。
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
          bool pcm_ok = true;
          tts_.set_event_callback([&](const BackendEvent& e) {
            if (!is_active() || e.kind != BackendEvent::Kind::kPcm) {
              return;
            }
            if (result.tts_first_pcm_ms < 0) {
              result.tts_first_pcm_ms = elapsed_ms();
            }
            if (!push_pcm(e.pcm)) {
              pcm_ok = false;
            }
          });
          tts_.synthesize(sentence);
          if (!pcm_ok) {
            // 取消或 sink 已失败由各自路径收尾；其余情况标记 TTS 失败，
            // 让主线程停止继续喂文本并返回明确错误。
            if (!is_active() || sink_write_failed.load()) {
              return;
            }
            tts_failed.store(true);
            return;
          }
          if (config_.stage_delay.count() > 0) {
            std::this_thread::sleep_for(config_.stage_delay);
          }
        }
      } catch (...) {
        tts_failed.store(true);
        tts_exception = std::current_exception();
      }
    });

    // 写出工作线程：PCM 帧 → sink（取消后滞留帧不写出）。
    sink_thread = std::thread([&] {
      std::vector<std::int16_t> frame;
      bool committed = input.commit_delay.count() <= 0;
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
        if (!committed) {
          if (!wait_for_commit()) {
            continue;  // 观察窗内被取消：旧预推理不提交首帧
          }
          committed = true;
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

    // ---------- 1. 输入阶段：WAV → ASR（Listening）----------
    state_.store(State::kListening);
    std::string query;
    if (input.mode == PipelineInput::Mode::kWav) {
      std::vector<std::int16_t> samples;
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
      state_.store(State::kRouting);
    } else {
      // 文本模式：无音频输入，Listening 阶段为空。连续采集已在会话外
      // 完成识别时，final 文本也从这条路径进入路由。
      state_.store(State::kRouting);
      query = input.text;
      result.asr_text = query;
      result.input_end_ms = elapsed_ms();
      result.asr_final_ms = result.input_end_ms;
      if (query.empty()) {
        result.error = "空文本请求";
      }
    }

    // ---------- 2. 路由阶段（Routing → 直答/Thinking）----------
    if (result.error.empty() && !cancelled_.load() && !timed_out()) {
      const auto decision = router_.route(query);
      result.route = rag::to_string(decision.level);

      if (decision.level == rag::RouteLevel::kL0 ||
          decision.level == rag::RouteLevel::kL1) {
        // L0/L1：绕过 LLM，直答文本进入 TTS。
        state_.store(State::kSpeaking);
        result.final_text = decision.answer;
        feed_answer_sentences(decision.answer);
      } else {
        // L2/L3：调用 LLM，token 流经分句器进入文本队列；TTS 工作线程
        // 已在 generate 之前启动，因此首句不会被整段生成阻塞。
        state_.store(State::kThinking);
        bool feed_stopped = false;
        common::SentenceChunker chunker(config_.text_chunk_max_bytes);
        llm_.set_event_callback([&](const BackendEvent& e) {
          if (!is_active() || feed_stopped) {
            return;  // 取消后或下游已停止：不再分句入队
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
              if (!push_text(s)) {
                feed_stopped = true;
                return;
              }
            }
          }
          if (config_.stage_delay.count() > 0) {
            std::this_thread::sleep_for(config_.stage_delay);
          }
        });
        llm_.generate(decision.prompt);
        if (!feed_stopped && is_active()) {
          for (const auto& s : chunker.flush()) {
            if (!push_text(s)) {
              break;
            }
          }
        }
        state_.store(State::kSpeaking);
      }
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

  if (tts_exception) {
    try {
      std::rethrow_exception(tts_exception);
    } catch (const std::exception& e) {
      result.error = std::string("TTS 阶段异常: ") + e.what();
    } catch (...) {
      result.error = "TTS 阶段异常";
    }
  } else if (tts_failed.load() && result.error.empty()) {
    result.error = "TTS 工作线程异常退出";
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
      state_.store(State::kIdle);
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
    state_.store(State::kCancelling);
    state_.store(State::kIdle);
  }

  if (result.ok) {
    result.wav_complete = config_.output_mode == "wav";
    result.playback_complete = config_.output_mode == "alsa";
  }

  // 统计与证据（工作线程已 join，无数据竞争）。
  result.text_queue_peak = text_queue.peak();
  result.pcm_queue_peak = pcm_queue.peak();
  result.total_ms = elapsed_ms();
  return result;
}

void SessionPipeline::cancel() {
  cancel_generation();
  asr_.cancel();
}

void SessionPipeline::cancel_generation() {
  // 递增世代：后续（含在途）回调全部失去活动性；新请求获得新世代。
  generation_.fetch_add(1);
  cancelled_.store(true);
  llm_.cancel();
  tts_.cancel();
  commit_cv_.notify_all();
  if (state_.load() != State::kIdle) {
    state_.store(State::kCancelling);
  }
}

const char* SessionPipeline::state_name() const {
  return StateName(state_.load());
}


std::string SessionPipeline::make_wav_path(
    const std::string& request_id) const {
  std::error_code ec;
  std::filesystem::create_directories(config_.output_dir, ec);
  return config_.output_dir + "/session_" + text_hash(request_id) + ".wav";
}

}  // namespace slotnexus::session
