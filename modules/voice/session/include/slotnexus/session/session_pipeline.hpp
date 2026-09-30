// Session 编排管线：固定输入 → ASR → L0-L3 路由 →（LLM）→ 分句 → TTS → PCM 输出。
// Author: Caden
//
// 编排职责（不做模型推理）：
//   - 一次 run() = 一个世代（generation）：WAV/文本输入、路由决策、
//     LLM token 流按句子分句进入有界文本队列，TTS 消费后按 PCM 帧进入
//     有界 PCM 队列，最后写入 IAudioSink（WAV/ALSA）；
//   - 有界队列：容量由配置给定，满队列按 queue_push_timeout 重试；
//     下游停摆或取消/超时时明确失败，不静默丢弃；
//   - 取消传播：cancel() 递增 generation 并向三个后端传播 cancel；
//     所有 token/PCM 事件在入队前做 (generation, request_id) 双检查，
//     旧世代或旧请求的数据一律不进入队列与输出（晚到消息过滤）；
//   - 状态机：Idle→Listening→Routing→Thinking→Speaking→Idle，取消路径
//     →Cancelling→Idle，轨迹记录在结果中。
//
// 线程模型：run() 在调用线程上驱动 ASR/路由/LLM；TTS 与写出工作线程在
// 路由/生成前就创建，首个可播片段进入文本队列后立即被消费；cancel() 可被
// 任意线程并发调用。同一时刻只允许一个 run 在途。
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "slotnexus/backend/i_asr_backend.hpp"
#include "slotnexus/backend/i_audio_sink.hpp"
#include "slotnexus/backend/i_llm_backend.hpp"
#include "slotnexus/backend/i_tts_backend.hpp"
#include "slotnexus/common/bounded_queue.hpp"
#include "slotnexus/rag/router.hpp"

namespace slotnexus::session {

// 管线配置：队列容量与满队列行为由调用方（session_node）从配置文件注入。
struct PipelineConfig {
  std::size_t text_queue_capacity = 8;       // 待合成句子队列容量
  std::size_t pcm_queue_capacity = 32;       // 待写出 PCM 帧队列容量
  std::size_t text_chunk_max_bytes = 60;     // 单个待合成片段上限；0 = 只按标点切
  std::chrono::milliseconds queue_push_timeout{50};  // 满队列重试间隔，不丢弃
  std::chrono::milliseconds stage_delay{0};  // 测试仪表：阶段人工延时（模拟流式）
  std::string output_dir = "session-out";    // WAV 输出目录
  // 最小合成时长：Fake TTS 按文本字节数产出帧（32 字节/帧），短回答
  // 只有 20-200ms（人耳"闪一下"）。输出不足该时长时补静音帧到该时长，
  // 保证演示音频可听；0 表示不补齐（测试与单元场景保持原样）。
  std::chrono::milliseconds tts_min_duration{0};
  // 输出目标语义：wav = 文件（回归/内容复核），alsa = 实时播放。
  // sink 具体实现由 SessionNode 的 SinkFactory 决定；这里只用于结果
  // 语义、WAV 路径生成和“真实播完”与“文件写完”的区分。
  std::string output_mode = "wav";
};

// 一次运行（一个请求）的输入。
struct PipelineInput {
  enum class Mode { kText, kWav };
  Mode mode = Mode::kText;
  std::string text;      // kText：文本直接进入路由
  std::string wav_path;  // kWav：WAV → ASR → 路由
};

// 运行结果：路由证据、统计与状态机轨迹（验收/证据记录用）。
struct PipelineResult {
  bool ok = false;
  bool cancelled = false;
  std::string error;
  std::string route;          // "l0"/"l1"/"l2"/"l3"
  std::string asr_text;       // 本轮 ASR 最终文本；文本模式等于输入文本
  std::string final_text;     // 回答文本（L0/L1 直答或 LLM 输出）
  std::string wav_path;       // WAV 路径；ALSA 模式为空（取消时可能为部分数据）
  std::string output_mode = "wav";  // wav / alsa
  bool audio_delivered = false;     // sink 已接受本次提交的 PCM
  bool wav_complete = false;        // WAV 头回填并关闭完成（文件写完）
  bool playback_complete = false;   // ALSA drain 完成（真实播放完）
  std::string sink_error;           // sink 打开/写入/关闭错误
  std::size_t token_count = 0;        // 本世代实际入列统计前的 token 数
  std::size_t pcm_frames = 0;         // 实际写入 sink 的 PCM 帧数
  std::size_t text_queue_peak = 0;    // 文本队列峰值（验收：≤ 容量）
  std::size_t pcm_queue_peak = 0;     // PCM 队列峰值（验收：≤ 容量）
  std::size_t generation = 0;         // 本次运行世代

  // 阶段耗时（相对 run 开始，steady_clock，未发生为 -1）。
  // WAV 固定输入先给 input_end_ms（喂入结束），ASR final、LLM 首
  // token、首可播文本、TTS 首 PCM、首帧提交 sink 和 sink close 分别记录，
  // 供启动/语音阶段基线对比；软件提交时间不等于真实扬声器首音。
  std::int64_t input_end_ms = -1;        // 语音样本喂入结束
  std::int64_t asr_final_ms = -1;        // ASR final 回调
  std::int64_t llm_first_token_ms = -1;  // LLM 首个 token
  std::int64_t first_text_ms = -1;       // 首个可播文本入队
  std::int64_t tts_first_pcm_ms = -1;    // TTS 首帧 PCM
  std::int64_t first_output_ms = -1;     // 首帧提交 sink
  std::int64_t output_complete_ms = -1;  // sink close 完成
  std::int64_t total_ms = -1;            // run 返回前总耗时
};

// 编排管线：后端与路由经构造函数注入（Node 只依赖接口与 Router）。
class SessionPipeline {
 public:
  // sink 工厂：每次运行按输出 WAV 路径创建一个 sink（per-request 输出）。
  using SinkFactory = std::function<std::unique_ptr<backend::IAudioSink>(
      const std::string& wav_path)>;

  SessionPipeline(PipelineConfig config, rag::Router& router,
                  backend::IAsrBackend& asr, backend::ILlmBackend& llm,
                  backend::ITtsBackend& tts, SinkFactory sink_factory);
  ~SessionPipeline() = default;

  SessionPipeline(const SessionPipeline&) = delete;
  SessionPipeline& operator=(const SessionPipeline&) = delete;

  // 同步运行一次完整链路（阻塞直到输出完成或取消/超时）；
  // deadline.count() <= 0 表示不限时。已有在途 run 时返回错误结果。
  PipelineResult run(const PipelineInput& input, const std::string& request_id,
                     std::chrono::milliseconds deadline);

  // 取消在途运行（线程安全）：递增 generation 并传播到三个后端。
  void cancel();

  // 状态机快照（taskinfo/日志用）。
  const char* state_name() const;

 private:
  // 管线阶段状态与合法迁移；轨迹用于 taskinfo/日志。状态与轨迹只在
  // 互斥量保护下读写，cancel 可与运行线程并发。
  enum class State {
    kIdle,
    kListening,
    kRouting,
    kThinking,
    kSpeaking,
    kCancelling
  };

  static const char* StateName(State state);
  void reset_state();

  // 输出 WAV 路径：output_dir/session_<request_id 哈希>.wav（确定性）。
  std::string make_wav_path(const std::string& request_id) const;

  PipelineConfig config_;
  rag::Router& router_;
  backend::IAsrBackend& asr_;
  backend::ILlmBackend& llm_;
  backend::ITtsBackend& tts_;
  SinkFactory sink_factory_;

  std::atomic<State> state_{State::kIdle};

  std::atomic<bool> running_{false};
  std::atomic<bool> cancelled_{false};
  std::atomic<std::uint64_t> generation_{0};

  // 本次运行的世代与请求（驱动线程在启动工作线程前写入；
  // 线程创建建立 happens-before，运行期间只读）。
  std::uint64_t active_generation_ = 0;
  std::string active_request_id_;
};

}  // namespace slotnexus::session
