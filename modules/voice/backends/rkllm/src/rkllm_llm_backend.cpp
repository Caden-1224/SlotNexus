// RkllmBackend 实现：封装 RKLLM Runtime C API（librkllmrt.so）。
// Author: Caden
//
// 当前使用的 RKLLM 调用序列：
//   rkllm_createDefaultParam → 采样/运行参数 → rkllm_init →
//   rkllm_run_async（异步，userdata 传 Impl*，经回调回传）→
//   generate 泵队列等 FINISH/ERROR → rkllm_destroy。
// RKLLM 回调来自厂商内部线程：回调只把 {generation, state, text 拷贝} 压入
// 互斥队列并 notify（速拷，不阻塞厂商线程）；BackendEvent 一律由 generate
// 的调用线程泵队列时投递。取消置位后泵循环立即停发（旧 token 全过滤，
// 含已入队未投递的），厂商 rkllm_abort 尽力而为，返回值不作为依据。
//
// 模型无关性：全部采样/运行参数来自 RkllmOptions；思考段过滤由
// ReasoningFilter 承担（reasoning_end_tag 为空即直通）。
#include <cstddef>
#include <cstdint>
#include "rkllm.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include "slotnexus/backend/rkllm/rkllm_llm_backend.hpp"
#include "slotnexus/backend/rkllm/rkllm_reasoning_filter.hpp"

namespace slotnexus::backend::rkllm {

namespace {

// 泵循环取消响应粒度：cancelled 置位后最多约 20 ms 内停发。
constexpr std::chrono::milliseconds kPumpWaitMs(20);

}  // namespace

struct RkllmBackend::Impl {
  // 厂商回调 → 受控队列的一个元素（state + text 拷贝，回调线程只做这个）。
  struct Item {
    std::uint64_t generation;  // 产生该事件的会话代号（用于丢弃残留）
    LLMCallState state;
    std::string text;
    // 回调线程入队时刻，用于测量下游交付前的队列等待。
    std::chrono::steady_clock::time_point enqueued_at{};
    // 仅 FINISH/ERROR 事件携带厂商最终性能统计；按值拷贝，不保存 SDK 指针。
    bool has_perf = false;
    RKLLMPerfStat perf{};
  };

  // 单轮 generate 的性能观测：厂商 perf + 队列等待 + 应用侧交付时间。
  // 只由 generate 调用线程读写；callback 线程只把数据拷进 Item。
  struct Metrics {
    std::chrono::steady_clock::time_point start;
    std::chrono::steady_clock::time_point first_vendor;
    std::chrono::steady_clock::time_point first_deliver;
    std::chrono::steady_clock::time_point last_deliver;
    bool has_first_vendor = false;
    bool has_first_deliver = false;
    bool has_deliver = false;
    bool has_perf = false;
    RKLLMPerfStat perf{};
    std::uint64_t queue_items = 0;
    std::uint64_t queue_wait_total_us = 0;
    std::uint64_t queue_wait_max_us = 0;
    std::uint64_t text_events = 0;
    std::uint64_t text_bytes = 0;
  };

  explicit Impl(const RkllmOptions& options) : options_(options) {
    RKLLMParam param = rkllm_createDefaultParam();
    param.model_path = options_.model_path.c_str();
    param.top_k = options_.top_k;
    param.top_p = options_.top_p;
    param.temperature = options_.temperature;
    param.repeat_penalty = options_.repeat_penalty;
    param.frequency_penalty = options_.frequency_penalty;
    param.presence_penalty = options_.presence_penalty;
    param.max_new_tokens = options_.max_new_tokens;
    param.max_context_len = options_.max_context_len;
    param.skip_special_token = options_.skip_special_token;
    param.ignore_eos_token = options_.ignore_eos_token;
    // 异步：rkllm_run_async 立即返回，回调由厂商内部线程按 token 流式触发
    // （同步模式下 run 阻塞、事件只能批量落地，无法流式投递也无法中途取消）。
    param.is_async = true;
    param.extend_param.base_domain_id = options_.base_domain_id;
    param.extend_param.embed_flash = options_.embed_flash ? 1 : 0;
    param.extend_param.enabled_cpus_num =
        static_cast<int8_t>(options_.enabled_cpus_num);
    param.extend_param.enabled_cpus_mask =
        static_cast<uint32_t>(options_.enabled_cpus_mask);

    LLMHandle h = nullptr;
    int ret = 0;
#if defined(SLOTNEXUS_RKLLM_INIT_CALLBACK_STRUCT)
    // 新接口（rknn-llm 1.2.0 之后）：回调注册在 RKLLMCallback 结构里。
    RKLLMCallback callback = {};
    callback.result_callback = &Impl::on_vendor_result;
    // rkllm_run_async 的第 4 个参数优先于这里；保留 nullptr 作为兜底。
    callback.result_userdata = nullptr;
    ret = rkllm_init(&h, &param, &callback);
#else
    // 旧接口（rknn-llm 1.2.0 build 2025-04-08）：直接传裸函数指针。
    ret = rkllm_init(&h, &param, &Impl::on_vendor_result);
#endif
    if (ret != 0 || h == nullptr) {
      throw std::runtime_error("rkllm_init 失败 ret=" + std::to_string(ret) +
                               ": " + options_.model_path);
    }
    handle = h;
    // 对话模板用模型自带（.rkllm 导出时打包）。早期覆盖为纯 ｜User｜/｜Assistant｜
    // 会在生成时反复输出模板符号（实测：100 token 全是 "｜ User ｜｜
    // Assistant ｜"），勿再覆盖。
  }

  ~Impl() {
    if (handle != nullptr) {
      cancelled.store(true);
      if (running.load()) {
        rkllm_abort(handle);  // 尽力而为；过滤靠 cancelled
      }
      rkllm_destroy(handle);
    }
  }

  // 厂商回调（rkllm_run 的 userdata 透传本实例）：速拷入队，不投递事件。
  // 两种 SDK 的差异只在返回值类型（新接口的 LLMResultCallback 返回 int，
  // 返回 1 表示挂起本次推理；这里始终返回 0 继续生成），回调体共用。
  static void handle_vendor_result(RKLLMResult* result, void* userdata,
                                   LLMCallState state) {
    auto* self = static_cast<Impl*>(userdata);
    if (self == nullptr) {
      return;
    }
    Item item;
    item.enqueued_at = std::chrono::steady_clock::now();
    item.state = state;
    if (state == RKLLM_RUN_NORMAL && result != nullptr &&
        result->text != nullptr) {
      item.text = result->text;
    }
    // 最终回调携带 perf；立即拷贝，避免回调返回后 SDK 复用/释放数据。
    if (result != nullptr &&
        (state == RKLLM_RUN_FINISH || state == RKLLM_RUN_ERROR)) {
      item.perf = result->perf;
      item.has_perf = true;
    }
    {
      std::lock_guard<std::mutex> lk(self->mu);
      item.generation = self->generation;
      self->queue.push_back(std::move(item));
    }
    self->cv.notify_one();
  }

#if defined(SLOTNEXUS_RKLLM_INIT_CALLBACK_STRUCT)
  static int on_vendor_result(RKLLMResult* result, void* userdata,
                              LLMCallState state) {
    handle_vendor_result(result, userdata, state);
    return 0;  // 不挂起推理
  }
#else
  static void on_vendor_result(RKLLMResult* result, void* userdata,
                               LLMCallState state) {
    handle_vendor_result(result, userdata, state);
  }
#endif

  // 泵队列：只投递与 my_generation 匹配的事件，且每次投递前复查取消；
  // cancelled 置位立即返回 false（generate 不得再产出任何事件）。
  // 正常结束（FINISH / ERROR）返回 true，delivered 为实际下发给下游的文本
  // 拼接（已剔除思考段），供 kDone 使用。
  bool pump(std::uint64_t my_generation, const EventCallback& cb,
            std::string* delivered, Metrics* metrics) {
    delivered->clear();
    std::string pending_waiting;  // WAITING 状态携带的 UTF-8 半字符
    // 思考段过滤（每轮 generate 独立状态）：见 ReasoningFilter 头文件注释。
    ReasoningFilter filter(options_.reasoning_end_tag,
                           options_.reasoning_max_buffer_bytes);

    // 真正投递一段已过滤文本；空结果不下发。返回 false 表示已取消，需停发。
    const auto deliver = [&](const std::string& out) -> bool {
      if (out.empty()) {
        return true;
      }
      if (cancelled.load()) {
        return false;
      }
      *delivered += out;
      if (cb) {
        const auto t0 = std::chrono::steady_clock::now();
        cb({BackendEvent::Kind::kToken, out, {}});
        const auto t1 = std::chrono::steady_clock::now();
        if (metrics != nullptr) {
          if (!metrics->has_first_deliver) {
            metrics->first_deliver = t0;
            metrics->has_first_deliver = true;
          }
          metrics->last_deliver = t1;
          metrics->has_deliver = true;
          ++metrics->text_events;
          metrics->text_bytes += out.size();
        }
      }
      return true;
    };

    // 把过滤结果投递给下游；空结果不下发。
    const auto emit = [&](const std::string& raw) -> bool {
      if (cancelled.load()) {
        return false;
      }
      std::string out = filter.accept(raw);
      if (out.empty()) {
        return true;
      }
      return deliver(out);
    };

    for (;;) {
      // 关键：只在取队列时持锁。取出后立即解锁，过滤、下游 cb() 都在锁外
      // 执行；否则下游处理慢会阻塞厂商回调线程入队，进而拖慢生成。
      std::deque<Item> batch;
      {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait_for(lk, kPumpWaitMs,
                    [this] { return cancelled.load() || !queue.empty(); });
        if (cancelled.load()) {
          return false;
        }
        if (queue.empty()) {
          continue;
        }
        batch.swap(queue);  // O(1)：把当前已到达的事件整批拿到锁外处理
      }
      while (!batch.empty()) {
        if (cancelled.load()) {
          return false;  // 停发：旧 token 过滤
        }
        Item item = std::move(batch.front());
        batch.pop_front();
        if (item.generation != my_generation) {
          continue;  // 上一会话残留（vendor 线程晚到），丢弃
        }
        if (metrics != nullptr) {
          if (item.has_perf) {
            metrics->has_perf = true;
            metrics->perf = item.perf;
          }
          if (!metrics->has_first_vendor) {
            metrics->first_vendor = item.enqueued_at;
            metrics->has_first_vendor = true;
          }
          const auto now = std::chrono::steady_clock::now();
          const auto wait_us =
              std::chrono::duration_cast<std::chrono::microseconds>(
                  now - item.enqueued_at)
                  .count();
          metrics->queue_wait_total_us +=
              static_cast<std::uint64_t>(wait_us > 0 ? wait_us : 0);
          if (static_cast<std::uint64_t>(wait_us) > metrics->queue_wait_max_us) {
            metrics->queue_wait_max_us = static_cast<std::uint64_t>(wait_us);
          }
          ++metrics->queue_items;
        }
        if (item.state == RKLLM_RUN_WAITING) {
          // 半截 UTF-8 字符：暂存，等下一个 NORMAL 补全后合并投递。
          pending_waiting += item.text;
        } else if (item.state == RKLLM_RUN_NORMAL) {
          std::string text = pending_waiting + item.text;
          pending_waiting.clear();
          if (!emit(text)) {
            return false;
          }
        } else {
          // FINISH / ERROR：本次 run 终止（generate 统一补 kDone）。
          if (!pending_waiting.empty()) {
            if (!emit(pending_waiting)) {
              return false;
            }
            pending_waiting.clear();
          }
          // 思考段未闭合（token 预算耗尽 / 生成出错）：回退放行缓冲内容，
          // 保证下游有输出可读（与过滤器的未闭合回退语义一致）。
          const std::string tail = filter.flush();
          if (!tail.empty()) {
            // 这不是错误，但意味着本轮回答（或其中一大段）被压到生成结束才
            // 下发，流式重叠失效。对根本不输出思考段的模型，根因是
            // reasoning_end_tag 配错了，因此显式提示一次。
            std::fprintf(
                stderr,
                "[rkllm] 本轮生成结束仍未出现思考段标记 \"%s\"，"
                "被缓冲的回答延迟到生成结束才下发；"
                "若该模型不输出思考段，请把 reasoning_end_tag 设为空\n",
                options_.reasoning_end_tag.c_str());
            if (!deliver(tail)) {
              return false;
            }
          }
          return true;
        }
      }
    }
  }

  // 打印本轮真实 / 应用侧指标。只读，不改变生成行为，便于板端基线对照。
  void log_metrics(const Metrics& m, std::chrono::steady_clock::time_point end,
                   std::chrono::steady_clock::time_point done_return,
                   std::size_t prompt_bytes) const {
    const auto ms = [](std::chrono::steady_clock::time_point a,
                       std::chrono::steady_clock::time_point b) {
      return std::chrono::duration<double, std::milli>(b - a).count();
    };
    const double total_ms = ms(m.start, end);
    const double done_return_ms = ms(m.start, done_return);
    const double first_vendor_ms =
        m.has_first_vendor ? ms(m.start, m.first_vendor) : -1.0;
    const double first_deliver_ms =
        m.has_first_deliver ? ms(m.start, m.first_deliver) : -1.0;
    const double last_deliver_ms =
        m.has_deliver ? ms(m.start, m.last_deliver) : -1.0;
    const double queue_wait_avg_ms =
        m.queue_items
            ? static_cast<double>(m.queue_wait_total_us) /
                  static_cast<double>(m.queue_items) / 1000.0
            : 0.0;
    const double queue_wait_max_ms =
        static_cast<double>(m.queue_wait_max_us) / 1000.0;

    if (!m.has_perf) {
      std::fprintf(
          stderr,
          "[rkllm][perf] prompt_bytes=%zu perf=none queue_items=%llu "
          "queue_wait_avg_ms=%.3f queue_wait_max_ms=%.3f "
          "first_vendor_ms=%.2f first_deliver_ms=%.2f last_deliver_ms=%.2f "
          "total_ms=%.2f done_return_ms=%.2f text_events=%llu text_bytes=%llu\n",
          prompt_bytes, static_cast<unsigned long long>(m.queue_items),
          queue_wait_avg_ms, queue_wait_max_ms, first_vendor_ms,
          first_deliver_ms, last_deliver_ms, total_ms, done_return_ms,
          static_cast<unsigned long long>(m.text_events),
          static_cast<unsigned long long>(m.text_bytes));
      return;
    }

    const double prefill_tps =
        m.perf.prefill_time_ms > 0.0f
            ? static_cast<double>(m.perf.prefill_tokens) * 1000.0 /
                  static_cast<double>(m.perf.prefill_time_ms)
            : 0.0;
    const double decode_tps =
        m.perf.generate_time_ms > 0.0f
            ? static_cast<double>(m.perf.generate_tokens) * 1000.0 /
                  static_cast<double>(m.perf.generate_time_ms)
            : 0.0;
    std::fprintf(
        stderr,
        "[rkllm][perf] prompt_bytes=%zu prefill_ms=%.2f prefill_tokens=%d "
        "prefill_tps=%.2f generate_ms=%.2f generate_tokens=%d "
        "decode_tps=%.2f memory_mb=%.2f queue_items=%llu "
        "queue_wait_avg_ms=%.3f queue_wait_max_ms=%.3f "
        "first_vendor_ms=%.2f first_deliver_ms=%.2f last_deliver_ms=%.2f "
        "total_ms=%.2f done_return_ms=%.2f text_events=%llu text_bytes=%llu\n",
        prompt_bytes, m.perf.prefill_time_ms, m.perf.prefill_tokens, prefill_tps,
        m.perf.generate_time_ms, m.perf.generate_tokens, decode_tps,
        m.perf.memory_usage_mb, static_cast<unsigned long long>(m.queue_items),
        queue_wait_avg_ms, queue_wait_max_ms, first_vendor_ms,
        first_deliver_ms, last_deliver_ms, total_ms, done_return_ms,
        static_cast<unsigned long long>(m.text_events),
        static_cast<unsigned long long>(m.text_bytes));
  }

  RkllmOptions options_;  // 模型路径与全部采样/运行参数（值语义，持有副本）
  LLMHandle handle = nullptr;
  EventCallback cb;                 // 只由 set_event_callback / generate 使用
  std::atomic<bool> cancelled{false};
  std::atomic<bool> running{false};
  std::mutex mu;                    // 保护 queue / generation / cb 交接
  std::condition_variable cv;
  std::deque<Item> queue;
  std::uint64_t generation = 0;
};

RkllmBackend::RkllmBackend(RkllmOptions options) {
  impl_ = std::make_unique<Impl>(options);
}

RkllmBackend::~RkllmBackend() = default;

void RkllmBackend::set_event_callback(EventCallback cb) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  impl_->cb = std::move(cb);
  impl_->cancelled.store(false);
  impl_->queue.clear();  // 新会话：丢弃上一会话残留事件
}

void RkllmBackend::generate(const std::string& prompt) {
  std::uint64_t gen;
  EventCallback cb;
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    cb = impl_->cb;
    gen = ++impl_->generation;
  }
  if (!cb || impl_->cancelled.load()) {
    return;  // 取消后 generate 为空操作
  }

  RKLLMInput input;
  std::memset(&input, 0, sizeof(input));
  input.role = "user";  // 与模型自带对话模板配合；空 role 会绕过模板。
  input.input_type = RKLLM_INPUT_PROMPT;
  input.prompt_input = prompt.c_str();
  // 思考模式开关：Qwen3 系列据此决定是否输出思考段；对不支持该字段的模型
  // 无副作用（RKLLM 内部忽略）。
  input.enable_thinking = impl_->options_.enable_thinking;

  RKLLMInferParam infer;
  std::memset(&infer, 0, sizeof(infer));
  infer.mode = RKLLM_INFER_GENERATE;
  infer.keep_history = 0;
  // max_new_tokens <= 0 表示沿用 rkllm_init 时的取值。
  infer.max_new_tokens = 0;

  Impl::Metrics metrics;
  metrics.start = std::chrono::steady_clock::now();
  impl_->running.store(true);
  // rkllm_run_async 立即返回，回调由厂商内部线程按 token 流式触发，泵队列
  // 才能实时投递、生成中途才能取消。
  const int ret = rkllm_run_async(impl_->handle, &input, &infer, impl_.get());
  if (ret != 0) {
    // 本次生成未启动：不产出任何事件（与取消后空操作等价）。
    impl_->running.store(false);
    return;
  }
  std::string delivered;
  const bool normal_end = impl_->pump(gen, cb, &delivered, &metrics);
  const auto end = std::chrono::steady_clock::now();
  impl_->running.store(false);
  if (normal_end) {
    // kDone 携带实际下发文本（思考段已在 pump 内剔除）。
    cb({BackendEvent::Kind::kDone, std::move(delivered), {}});
  }
  const auto done_return = std::chrono::steady_clock::now();
  impl_->log_metrics(metrics, end, done_return, prompt.size());
}

void RkllmBackend::cancel() {
  impl_->cancelled.store(true);
  // 尽力而为中止厂商侧生成（返回码不可靠，token 过滤以 cancelled 为准）。
  if (impl_->running.load()) {
    rkllm_abort(impl_->handle);
  }
}

}  // namespace slotnexus::backend::rkllm
