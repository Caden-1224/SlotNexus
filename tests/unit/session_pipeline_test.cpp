// Session 编排管线单元测试：四类路由、WAV 固定输入、有界队列满丢弃、
// 取消传播（顽固后端晚到 token/PCM 过滤）、超时与单流语义。
//
// 验收对应：
//   - 取消后旧数据为 0（队列为空、sink 不再写入、计数归零）；
//   - 队列峰值不超过配置容量；
//   - 满队列行为明确（超时丢弃并计数）。
#include "voxorchestra/session/session_pipeline.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include "voxorchestra/backend/backend_event.hpp"
#include "voxorchestra/backend/fake/fake_audio_sink.hpp"
#include "voxorchestra/backend/fake/fake_asr_backend.hpp"
#include "voxorchestra/backend/fake/fake_llm_backend.hpp"
#include "voxorchestra/backend/fake/fake_tts_backend.hpp"
#include "voxorchestra/rag/router.hpp"

namespace sess = voxorchestra::session;
namespace back = voxorchestra::backend;
namespace fake = voxorchestra::backend::fake;
namespace rg = voxorchestra::rag;

using namespace std::chrono_literals;

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

// 与 rag_test 相同的确定性小语料（得分已在 rag_test 中标定：
// "the cat"→1.334、单独高频词→0.66 左右）。
struct Fixture {
  rg::Bm25Index index;
  std::vector<rg::KnowledgeEntry> entries;
  rg::Router router;
  std::string out_dir;

  Fixture()
      : index(build_index()),
        entries({{"e1", "the cat sat on the mat"},
                 {"e2", "the dog ran in the park"},
                 {"e3", "a cat and a dog"}}),
        router(index, entries, build_config()),
        out_dir(tmp_dir()) {}

  static rg::Bm25Index build_index() {
    rg::Bm25Index i;
    i.add_document("the cat sat on the mat");
    i.add_document("the dog ran in the park");
    i.add_document("a cat and a dog");
    i.build();
    return i;
  }

  static rg::RouterConfig build_config() {
    rg::RouterConfig c;
    c.direct_threshold = 0.7;
    c.context_threshold = 0.4;
    c.top_k = 2;
    return c;
  }

  static std::string tmp_dir() {
    const std::string d =
        std::filesystem::temp_directory_path().string() + "/vox_session_" +
        std::to_string(::getpid());
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
  }
};

// 记录调用方：记录 prompt 并转发给 FakeLlm（验证 L2 上下文是否注入）。
class RecordingLlm final : public back::ILlmBackend {
 public:
  std::vector<std::string> prompts;
  back::EventCallback cb_;

  void set_event_callback(back::EventCallback cb) override {
    cb_ = std::move(cb);
  }

  void generate(const std::string& prompt) override {
    prompts.push_back(prompt);
    fake::FakeLlmBackend inner;
    inner.set_event_callback(cb_);
    inner.generate(prompt);
  }

  void cancel() override {}
};

// 空识别后端：收到有效 PCM，但 final 文本为空（现场静音/漏识别场景）。
class EmptyFinalAsr final : public back::IAsrBackend {
 public:
  void set_event_callback(back::EventCallback cb) override {
    cb_ = std::move(cb);
  }

  void feed_audio(const std::vector<std::int16_t>&, bool is_last) override {
    if (is_last && cb_) {
      cb_({back::BackendEvent::Kind::kFinal, "", {}});
    }
  }

  void cancel() override {}

 private:
  back::EventCallback cb_;
};

// 顽固大模型：无视 cancel，按固定节奏持续产出 token（模拟真实 SDK 回调线程）。
class SlowStubbornLlm final : public back::ILlmBackend {
 public:
  SlowStubbornLlm(int tokens, std::chrono::milliseconds gap)
      : tokens_(tokens), gap_(gap) {}

  back::EventCallback cb_;

  void set_event_callback(back::EventCallback cb) override {
    cb_ = std::move(cb);
  }

  void generate(const std::string& /*prompt*/) override {
    for (int i = 0; i < tokens_; ++i) {
      if (cb_) {
        cb_({back::BackendEvent::Kind::kToken, "tok" + std::to_string(i), {}});
      }
      std::this_thread::sleep_for(gap_);
    }
    if (cb_) {
      cb_({back::BackendEvent::Kind::kDone, {}, {}});
    }
  }

  void cancel() override {}  // 顽固：取消后仍继续产出（晚到消息来源）

 private:
  int tokens_;
  std::chrono::milliseconds gap_;
};

// 顽固 TTS：无视 cancel，按固定节奏持续产出 PCM 帧。
class SlowStubbornTts final : public back::ITtsBackend {
 public:
  SlowStubbornTts(int frames, std::chrono::milliseconds gap)
      : frames_(frames), gap_(gap) {}

  back::EventCallback cb_;

  void set_event_callback(back::EventCallback cb) override {
    cb_ = std::move(cb);
  }

  void synthesize(const std::string& /*text*/) override {
    for (int i = 0; i < frames_; ++i) {
      std::vector<std::int16_t> pcm(static_cast<std::size_t>(back::kFrameSamples),
                                    600);
      if (cb_) {
        cb_({back::BackendEvent::Kind::kPcm, {}, std::move(pcm)});
      }
      std::this_thread::sleep_for(gap_);
    }
    if (cb_) {
      cb_({back::BackendEvent::Kind::kDone, {}, {}});
    }
  }

  void cancel() override {}  // 顽固：取消后仍继续产出

 private:
  int frames_;
  std::chrono::milliseconds gap_;
};

// 多句大模型：一次产出 30 个以句号结尾的句子（触发文本队列背压）。
class ManySentencesLlm final : public back::ILlmBackend {
 public:
  explicit ManySentencesLlm(int sentences) : sentences_(sentences) {}

  back::EventCallback cb_;

  void set_event_callback(back::EventCallback cb) override {
    cb_ = std::move(cb);
  }

  void generate(const std::string& /*prompt*/) override {
    std::string all;
    for (int i = 0; i < sentences_; ++i) {
      const std::string tok = "句子" + std::to_string(i) + "。";
      if (cb_) {
        cb_({back::BackendEvent::Kind::kToken, tok, {}});
      }
      all += tok;
    }
    if (cb_) {
      cb_({back::BackendEvent::Kind::kDone, all, {}});
    }
  }

  void cancel() override {}

 private:
  int sentences_;
};

// 计数 sink：统计写入帧数（不落盘）。
class CountingSink final : public back::IAudioSink {
 public:
  std::size_t frames = 0;
  std::size_t total_samples = 0;
  bool opened = false;
  bool closed = false;

  bool open() override {
    opened = true;
    return true;
  }
  bool write_pcm(const std::vector<std::int16_t>& pcm) override {
    if (!opened) {
      return false;
    }
    ++frames;
    total_samples += pcm.size();
    return true;
  }
  bool close() override {
    closed = true;
    return true;
  }
};

// 写入测试 WAV（16kHz 单声道 16-bit，N 个采样）。
bool write_test_wav(const std::string& path, std::uint32_t sample_rate,
                    std::size_t samples) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  const std::uint32_t data_bytes = static_cast<std::uint32_t>(samples * 2);
  const auto put16 = [&](std::uint16_t v) {
    out.put(static_cast<char>(v & 0xFF));
    out.put(static_cast<char>((v >> 8) & 0xFF));
  };
  const auto put32 = [&](std::uint32_t v) {
    out.put(static_cast<char>(v & 0xFF));
    out.put(static_cast<char>((v >> 8) & 0xFF));
    out.put(static_cast<char>((v >> 16) & 0xFF));
    out.put(static_cast<char>((v >> 24) & 0xFF));
  };
  out.write("RIFF", 4);
  put32(36 + data_bytes);
  out.write("WAVE", 4);
  out.write("fmt ", 4);
  put32(16);
  put16(1);
  put16(1);
  put32(sample_rate);
  put32(sample_rate * 2);
  put16(2);
  put16(16);
  out.write("data", 4);
  put32(data_bytes);
  for (std::size_t i = 0; i < samples; ++i) {
    put16(static_cast<std::uint16_t>((i * 7) % 1000 - 500));
  }
  return out.good();
}

sess::PipelineConfig base_config(const Fixture& f) {
  sess::PipelineConfig c;
  c.output_dir = f.out_dir;
  return c;
}

// ---------- 四类路由走对路径 ----------

void test_four_routes() {
  Fixture f;
  fake::FakeAsrBackend asr;
  RecordingLlm llm;
  fake::FakeTtsBackend tts;
  auto sink_factory = [](const std::string&) {
    return std::make_unique<CountingSink>();
  };
  sess::SessionPipeline pipe(base_config(f), f.router, asr, llm, tts,
                             sink_factory);

  // L0：控制词 → 直答、不调 LLM。
  auto r0 = pipe.run({sess::PipelineInput::Mode::kText, "停止播放", ""},
                     "r-l0", 0ms);
  CHECK(r0.ok);
  CHECK(r0.route == "l0");
  CHECK(!r0.llm_called);
  CHECK(!r0.final_text.empty());
  CHECK(llm.prompts.empty());
  CHECK(r0.pcm_frames > 0);

  // L1：高置信直答、不调 LLM。
  auto r1 = pipe.run({sess::PipelineInput::Mode::kText, "the cat", ""},
                     "r-l1", 0ms);
  CHECK(r1.ok);
  CHECK(r1.route == "l1");
  CHECK(!r1.llm_called);
  CHECK(r1.final_text == "the cat sat on the mat");
  CHECK(r1.decision.chunks.front().id == "e1");

  // L2：带上下文调 LLM（prompt 注入 Top-2 知识块）。
  auto r2 = pipe.run({sess::PipelineInput::Mode::kText, "the", ""}, "r-l2",
                     0ms);
  CHECK(r2.ok);
  CHECK(r2.route == "l2");
  CHECK(r2.llm_called);
  CHECK(llm.prompts.size() == 1);
  CHECK(llm.prompts[0].find("the cat sat on the mat") != std::string::npos);
  CHECK(llm.prompts[0].find("the dog ran in the park") != std::string::npos);
  CHECK(!r2.final_text.empty());

  // L3：闲聊无上下文调 LLM。
  auto r3 = pipe.run({sess::PipelineInput::Mode::kText, "hello world", ""},
                     "r-l3", 0ms);
  CHECK(r3.ok);
  CHECK(r3.route == "l3");
  CHECK(r3.llm_called);
  CHECK(llm.prompts.size() == 2);
  CHECK(llm.prompts[1] == "hello world");  // 未注入知识
  CHECK(r3.final_text == "hello world");

  // 队列峰值与容量约束（所有路径）。
  for (const auto* r : {&r0, &r1, &r2, &r3}) {
    CHECK(r->text_queue_peak <= base_config(f).text_queue_capacity);
    CHECK(r->pcm_queue_peak <= base_config(f).pcm_queue_capacity);
  }
  // 状态机轨迹：L1 与 L3 路径的完整迁移。
  CHECK(r1.transitions.size() == 4);
  CHECK(r1.transitions[0] == "idle--audio_start-->listening");
  CHECK(r1.transitions[3] == "speaking--tts_done-->idle");
  CHECK(r3.transitions.size() == 5);
  CHECK(r3.transitions[2] == "routing--route_l2_l3-->thinking");
  std::cout << "  [ok] 四类路由：L0/L1 直答绕 LLM，L2/L3 走 LLM（L2 带上下文）"
            << std::endl;
}

// ---------- WAV 固定输入完整链路 ----------

void test_wav_input_pipeline() {
  Fixture f;
  const std::string wav_path = f.out_dir + "/input.wav";
  // 3 帧 × 320 采样：FakeAsr 产出 "第1帧(320) 第2帧(320) 第3帧(320)"。
  CHECK(write_test_wav(wav_path, back::kSampleRateHz, 960));
  fake::FakeAsrBackend asr;
  RecordingLlm llm;
  fake::FakeTtsBackend tts;
  auto sink_factory = [&](const std::string& p) {
    return std::make_unique<fake::FakeAudioSink>(p);  // 真实 WAV 输出
  };
  sess::SessionPipeline pipe(base_config(f), f.router, asr, llm, tts,
                             sink_factory);

  const auto r = pipe.run({sess::PipelineInput::Mode::kWav, "", wav_path},
                          "r-wav", 0ms);
  CHECK(r.ok);
  CHECK(r.route == "l3");  // 帧描述文本无知识命中
  CHECK(r.llm_called);
  // 路由规范化丢弃标点，LLM 回显规范化后的查询文本。
  CHECK(r.final_text == "第1帧320 第2帧320 第3帧320");
  CHECK(r.pcm_frames > 0);
  CHECK(std::filesystem::exists(r.wav_path));
  CHECK(r.text_queue_peak <= base_config(f).text_queue_capacity);
  CHECK(r.pcm_queue_peak <= base_config(f).pcm_queue_capacity);

  // 格式不符：44.1kHz 被拒绝。
  const std::string bad_path = f.out_dir + "/bad.wav";
  CHECK(write_test_wav(bad_path, 44100, 960));
  const auto rb =
      pipe.run({sess::PipelineInput::Mode::kWav, "", bad_path}, "r-bad", 0ms);
  CHECK(!rb.ok);
  CHECK(rb.error.find("16kHz") != std::string::npos);
  // 文件不存在。
  const auto rm = pipe.run({sess::PipelineInput::Mode::kWav, "",
                            f.out_dir + "/missing.wav"},
                           "r-miss", 0ms);
  CHECK(!rm.ok);
  CHECK(rm.error.find("无法打开") != std::string::npos);
  std::cout << "  [ok] WAV 固定输入：完整链路产出 WAV；格式与缺失错误明确"
            << std::endl;
}

// ---------- 有 PCM 但 ASR 未识别到文本：早失败，不调用 LLM/TTS ----------

void test_empty_asr_text_stops_pipeline() {
  Fixture f;
  EmptyFinalAsr asr;
  RecordingLlm llm;
  fake::FakeTtsBackend tts;
  auto sink_factory = [](const std::string&) {
    return std::make_unique<CountingSink>();
  };
  sess::SessionPipeline pipe(base_config(f), f.router, asr, llm, tts,
                             sink_factory);

  sess::PipelineInput input;
  input.mode = sess::PipelineInput::Mode::kMic;
  input.audio.assign(static_cast<std::size_t>(back::kFrameSamples), 100);
  const auto r = pipe.run(input, "r-empty-asr", 0ms);

  CHECK(!r.ok);
  CHECK(r.asr_text.empty());
  CHECK(r.error.find("ASR 未识别到文本") != std::string::npos);
  CHECK(!r.llm_called);
  CHECK(llm.prompts.empty());
  CHECK(r.pcm_frames == 0);
  std::cout << "  [ok] 空 ASR 文本：返回明确错误且不调用 LLM/TTS"
            << std::endl;
}

// ---------- 有界队列：满时超时丢弃并计数，峰值 ≤ 容量 ----------

void test_queue_full_drop() {
  Fixture f;
  fake::FakeAsrBackend asr;
  ManySentencesLlm llm(30);  // 30 个句子
  fake::FakeTtsBackend tts;
  auto sink_factory = [](const std::string&) {
    return std::make_unique<CountingSink>();
  };
  sess::PipelineConfig c = base_config(f);
  c.text_queue_capacity = 1;   // 极小的文本队列
  c.pcm_queue_capacity = 2;
  c.queue_push_timeout = 1ms;  // 满队列等待 1ms 即丢弃
  c.stage_delay = 5ms;         // TTS 消费慢于生产 → 背压
  sess::SessionPipeline pipe(c, f.router, asr, llm, tts, sink_factory);

  const auto r = pipe.run({sess::PipelineInput::Mode::kText, "the", ""},
                          "r-full", 0ms);
  CHECK(r.ok);
  CHECK(r.route == "l2");
  CHECK(r.token_count == 30);
  CHECK(r.dropped_sentences > 0);  // 满队列超时丢弃（明确行为）
  CHECK(r.text_queue_peak <= c.text_queue_capacity);
  CHECK(r.pcm_queue_peak <= c.pcm_queue_capacity);
  CHECK(r.pcm_frames > 0);
  std::cout << "  [ok] 满队列：丢弃计数、峰值不超过容量（" << r.dropped_sentences
            << " 句丢弃）" << std::endl;
}

// ---------- 取消传播：顽固 LLM 晚到 token 过滤 ----------

void test_cancel_mid_llm_late_tokens() {
  Fixture f;
  fake::FakeAsrBackend asr;
  SlowStubbornLlm llm(20, 10ms);  // 20 token × 10ms ≈ 200ms，无视取消
  fake::FakeTtsBackend tts;
  auto sink_factory = [](const std::string&) {
    return std::make_unique<CountingSink>();
  };
  sess::SessionPipeline pipe(base_config(f), f.router, asr, llm, tts,
                             sink_factory);

  std::atomic<bool> started{false};
  sess::PipelineResult r;
  std::thread runner([&] {
    started.store(true);
    r = pipe.run({sess::PipelineInput::Mode::kText, "hello world", ""},
                 "req-cancel", 0ms);
  });
  while (!started.load()) {
    std::this_thread::sleep_for(1ms);
  }
  std::this_thread::sleep_for(50ms);  // 等 LLM 产出约 5 个 token
  pipe.cancel();
  runner.join();

  CHECK(r.cancelled);
  CHECK(r.token_count > 0 && r.token_count < 20);  // 只接受取消前 token
  // 取消后旧数据为 0：sink 未写任何帧、无丢弃计数、队列峰值 ≤ 1
  // （取消前少量 token 形成的句子在收尾时被整体丢弃）。
  CHECK(r.pcm_frames == 0);
  CHECK(r.text_queue_peak <= 1);
  CHECK(r.pcm_queue_peak == 0);
  CHECK(r.dropped_sentences == 0);
  CHECK(r.dropped_pcm_frames == 0);
  // 状态机回到 Idle，轨迹含 cancel 路径。
  CHECK(pipe.state_name() == std::string("idle"));
  CHECK(!r.transitions.empty());
  CHECK(r.transitions.back() == "cancelling--cancel_complete-->idle");

  // 新请求不受旧世代影响：完整跑通且世代递增（cancel 也递增一次世代）。
  const auto r2 = pipe.run({sess::PipelineInput::Mode::kText, "hello world",
                            ""},
                           "req-new", 0ms);
  CHECK(r2.ok);
  CHECK(!r2.cancelled);
  CHECK(r2.generation == r.generation + 2);
  CHECK(r2.final_text == "tok0 tok1 tok2 tok3 tok4 tok5 tok6 tok7 tok8 tok9 "
                         "tok10 tok11 tok12 tok13 tok14 tok15 tok16 tok17 "
                         "tok18 tok19");
  CHECK(r2.pcm_frames > 0);
  std::cout << "  [ok] 取消传播（LLM）：晚到 token 全部丢弃，旧数据为 0，"
               "新请求世代隔离"
            << std::endl;
}

// ---------- 取消传播：顽固 TTS 晚到 PCM 过滤 ----------

void test_cancel_mid_tts_late_pcm() {
  Fixture f;
  fake::FakeAsrBackend asr;
  RecordingLlm llm;
  SlowStubbornTts tts(10, 10ms);  // 10 帧 × 10ms，无视取消
  auto sink_factory = [](const std::string&) {
    return std::make_unique<CountingSink>();
  };
  sess::SessionPipeline pipe(base_config(f), f.router, asr, llm, tts,
                             sink_factory);

  sess::PipelineResult r;
  std::thread runner([&] {
    r = pipe.run({sess::PipelineInput::Mode::kText, "the cat", ""},
                 "req-tts", 0ms);  // L1 直答，1 个句子进入 TTS
  });
  std::this_thread::sleep_for(50ms);  // TTS 已产出约 5 帧
  pipe.cancel();
  runner.join();

  CHECK(r.cancelled);
  CHECK(r.pcm_frames > 0 && r.pcm_frames < 10);  // 取消后不再写帧
  CHECK(r.dropped_pcm_frames == 0);  // 晚到帧在入队前被过滤，不计丢弃
  CHECK(pipe.state_name() == std::string("idle"));

  // 新请求不包含旧 PCM：帧数只来自新请求。
  const auto r2 = pipe.run({sess::PipelineInput::Mode::kText, "the cat", ""},
                           "req-tts-new", 0ms);
  CHECK(r2.ok);
  CHECK(r2.pcm_frames > 0);
  std::cout << "  [ok] 取消传播（TTS）：取消后不再写帧，晚到 PCM 全部过滤"
            << std::endl;
}

// ---------- 最小合成时长：短回答补静音帧到可听时长 ----------

void test_min_tts_duration_padding() {
  Fixture f;
  fake::FakeAsrBackend asr;
  RecordingLlm llm;
  fake::FakeTtsBackend tts;
  sess::PipelineConfig c = base_config(f);
  c.tts_min_duration = 1000ms;  // 不足 1 秒补静音帧
  sess::SessionPipeline pipe(
      c, f.router, asr, llm, tts, [&](const std::string& p) {
        return std::make_unique<fake::FakeAudioSink>(p);  // 真实 WAV
      });
  // L1 短回答（23 字节 → FakeTts 仅 1 帧 20ms）：补齐到 50 帧 = 1 秒。
  const auto r = pipe.run({sess::PipelineInput::Mode::kText, "the cat", ""},
                          "r-min", 0ms);
  CHECK(r.ok);
  CHECK(r.route == "l1");
  CHECK(r.pcm_frames == 50);  // 1000ms / 20ms
  // 输出 WAV 数据长度 = 50 × 320 × 2 字节。
  std::FILE* fh = std::fopen(r.wav_path.c_str(), "rb");
  CHECK(fh != nullptr);
  if (fh != nullptr) {
    std::fseek(fh, 0, SEEK_END);
    const long size = std::ftell(fh);
    std::fclose(fh);
    CHECK(size == 44 + 50 * 320 * 2);
  }
  // 默认配置（0）：不补齐（pcm_frames 保持 FakeTts 原生帧数）。
  sess::SessionPipeline pipe0(base_config(f), f.router, asr, llm, tts,
                              [](const std::string&) {
                                return std::make_unique<CountingSink>();
                              });
  const auto r0 = pipe0.run({sess::PipelineInput::Mode::kText, "the cat", ""},
                            "r-min0", 0ms);
  CHECK(r0.ok);
  CHECK(r0.pcm_frames == 1);  // 原生 1 帧，不补齐
  std::cout << "  [ok] 最小合成时长：短回答补齐到配置时长，默认配置不补齐"
            << std::endl;
}

// ---------- 超时与单流 ----------

void test_deadline_timeout() {
  Fixture f;
  fake::FakeAsrBackend asr;
  SlowStubbornLlm llm(20, 10ms);  // 200ms
  fake::FakeTtsBackend tts;
  auto sink_factory = [](const std::string&) {
    return std::make_unique<CountingSink>();
  };
  sess::SessionPipeline pipe(base_config(f), f.router, asr, llm, tts,
                             sink_factory);
  const auto r = pipe.run({sess::PipelineInput::Mode::kText, "hello world", ""},
                          "req-timeout", 60ms);
  CHECK(!r.ok);
  CHECK(!r.cancelled);
  CHECK(r.error.find("超时") != std::string::npos);
  CHECK(pipe.state_name() == std::string("idle"));  // 超时后回到 Idle
  std::cout << "  [ok] deadline：超时返回明确错误且状态机回到 Idle" << std::endl;
}

void test_single_flow_busy() {
  Fixture f;
  fake::FakeAsrBackend asr;
  SlowStubbornLlm llm(20, 10ms);
  fake::FakeTtsBackend tts;
  auto sink_factory = [](const std::string&) {
    return std::make_unique<CountingSink>();
  };
  sess::SessionPipeline pipe(base_config(f), f.router, asr, llm, tts,
                             sink_factory);

  std::thread runner([&] {
    pipe.run({sess::PipelineInput::Mode::kText, "hello world", ""},
             "req-busy", 0ms);
  });
  std::this_thread::sleep_for(30ms);
  const auto r2 = pipe.run({sess::PipelineInput::Mode::kText, "x", ""},
                           "req-busy-2", 0ms);
  CHECK(!r2.ok);
  CHECK(r2.error.find("在途") != std::string::npos);
  runner.join();
  std::cout << "  [ok] 单流：并发 run 返回明确拒绝" << std::endl;
}

}  // namespace

int main() {
  std::cout << "session_pipeline_test:" << std::endl;
  test_four_routes();
  test_wav_input_pipeline();
  test_empty_asr_text_stops_pipeline();
  test_queue_full_drop();
  test_cancel_mid_llm_late_tokens();
  test_cancel_mid_tts_late_pcm();
  test_min_tts_duration_padding();
  test_deadline_timeout();
  test_single_flow_busy();

  if (g_failures == 0) {
    std::cout << "session_pipeline_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "session_pipeline_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
