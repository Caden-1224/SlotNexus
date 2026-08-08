// RkllmBackend 实现：封装 RKLLM Runtime C API（librkllmrt.so）。
//
// 调用序列与 rkllm_smoke.cpp 一致（行为依据）：
//   rkllm_createDefaultParam → 采样参数 → rkllm_set_chat_template →
//   rkllm_init → rkllm_run（异步，userdata 传 Impl*，经回调回传）→
//   generate 泵队列等 FINISH/ERROR → rkllm_destroy。
// RKLLM 回调来自厂商内部线程：回调只把 {generation, state, text 拷贝} 压入
// 互斥队列并 notify（速拷，不阻塞厂商线程）；BackendEvent 一律由 generate
// 的调用线程泵队列时投递。取消置位后泵循环立即停发（旧 token 全过滤，
// 含已入队未投递的），厂商 rkllm_abort 尽力而为，返回值不作为依据。
#include <cstddef>
#include <cstdint>
#include "rkllm.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include "voxorchestra/backend/rkllm/rkllm_llm_backend.hpp"

namespace voxorchestra::backend::rkllm {

namespace {

// 采样参数参考值（与 rkllm_smoke.cpp / llm_demo.cpp 一致；板端实测校准见
// artifacts/llm-integration/）。CPU0|CPU2 两核沿用门禁基线（7.79 tok/s）。
constexpr float kTopP = 0.95f;
constexpr float kTemperature = 0.8f;
constexpr float kRepeatPenalty = 1.1f;
constexpr int kTopK = 1;
constexpr bool kSkipSpecialToken = true;
constexpr int kBaseDomainId = 0;
constexpr int kEmbedFlash = 1;
constexpr int kEnabledCpusNum = 2;
constexpr int kEnabledCpusMask = CPU0 | CPU2;
// 泵循环取消响应粒度：cancelled 置位后最多约 20 ms 内停发。
constexpr std::chrono::milliseconds kPumpWaitMs(20);

// DeepSeek-R1 输出含 <think>…</think> 思考段；下游消费（TTS 朗读）需要
// 的只是正式回答。取最后一个 "</think>" 之后的内容（trim 前导空白）；
// 思考段未闭合（token 预算耗尽）时回退原文，保证有内容可读。
constexpr char kThinkEndTag[] = "</think>";
constexpr std::size_t kThinkEndTagLen = sizeof(kThinkEndTag) - 1;

// DeepSeek-R1 输出含 <think>…</think> 思考段；下游消费（TTS 朗读）需要
// 的只是正式回答。取最后一个 "</think>" 之后的内容（trim 前导空白）；
// 思考段未闭合（token 预算耗尽）时回退原文，保证有内容可读。
std::string StripThink(const std::string& s) {
  const std::size_t pos = s.rfind(kThinkEndTag);
  if (pos == std::string::npos) {
    return s;
  }
  std::size_t start = pos + kThinkEndTagLen;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\n')) {
    ++start;
  }
  return s.substr(start);
}

}  // namespace

struct RkllmBackend::Impl {
  // 厂商回调 → 受控队列的一个元素（state + text 拷贝，回调线程只做这个）。
  struct Item {
    std::uint64_t generation;  // 产生该事件的会话代号（用于丢弃残留）
    LLMCallState state;
    std::string text;
  };

  Impl(const std::string& model_path, int max_new_tokens,
       int max_context_len) {
    RKLLMParam param = rkllm_createDefaultParam();
    param.model_path = model_path.c_str();
    param.top_k = kTopK;
    param.top_p = kTopP;
    param.temperature = kTemperature;
    param.repeat_penalty = kRepeatPenalty;
    param.frequency_penalty = 0.0f;
    param.presence_penalty = 0.0f;
    param.max_new_tokens = max_new_tokens;
    param.max_context_len = max_context_len;
    param.skip_special_token = kSkipSpecialToken;
    // 异步：rkllm_run 立即返回，回调由厂商内部线程按 token 流式触发
    // （默认 is_async=false 时 run 同步阻塞、事件只能批量落地，无法流式
    // 投递也无法在生成中途取消）。
    param.is_async = true;
    param.extend_param.base_domain_id = kBaseDomainId;
    param.extend_param.embed_flash = kEmbedFlash;
    param.extend_param.enabled_cpus_num = kEnabledCpusNum;
    param.extend_param.enabled_cpus_mask = kEnabledCpusMask;

    LLMHandle h = nullptr;
    const int ret = rkllm_init(&h, &param, &Impl::on_vendor_result);
    if (ret != 0 || h == nullptr) {
      throw std::runtime_error("rkllm_init 失败 ret=" + std::to_string(ret) +
                               ": " + model_path);
    }
    handle = h;
    // 对话模板用模型自带（.rkllm 导出时打包，DeepSeek-R1-Distill 为
    // ｜User｜…｜Assistant｜<think>\n）。早期覆盖为纯 ｜User｜/｜Assistant｜
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
  static void on_vendor_result(RKLLMResult* result, void* userdata,
                               LLMCallState state) {
    auto* self = static_cast<Impl*>(userdata);
    if (self == nullptr) {
      return;
    }
    Item item;
    item.state = state;
    if (state == RKLLM_RUN_NORMAL && result != nullptr &&
        result->text != nullptr) {
      item.text = result->text;
    }
    {
      std::lock_guard<std::mutex> lk(self->mu);
      item.generation = self->generation;
      self->queue.push_back(std::move(item));
    }
    self->cv.notify_one();
  }

  // 泵队列：只投递与 my_generation 匹配的事件，且每次投递前复查取消；
  // cancelled 置位立即返回 false（generate 不得再产出任何事件）。
  // 正常结束（FINISH / ERROR）返回 true，accumulated 为累计输出文本。
  bool pump(std::uint64_t my_generation, const EventCallback& cb,
            std::string* accumulated) {
    accumulated->clear();
    std::string pending_waiting;  // WAITING 状态携带的 UTF-8 半字符
    // R1 思考段过滤（每轮 generate 独立状态）：见 NORMAL 分支注释。
    std::string think_buf;
    bool think_done = false;
    for (;;) {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait_for(lk, kPumpWaitMs,
                  [this] { return cancelled.load() || !queue.empty(); });
      if (cancelled.load()) {
        return false;
      }
      while (!queue.empty()) {
        if (cancelled.load()) {
          return false;  // 停发：旧 token 过滤
        }
        Item item = std::move(queue.front());
        queue.pop_front();
        if (item.generation != my_generation) {
          continue;  // 上一会话残留（vendor 线程晚到），丢弃
        }
        if (item.state == RKLLM_RUN_WAITING) {
          // 半截 UTF-8 字符：暂存，等下一个 NORMAL 补全后合并投递。
          pending_waiting += item.text;
        } else if (item.state == RKLLM_RUN_NORMAL) {
          std::string text = pending_waiting + item.text;
          pending_waiting.clear();
          *accumulated += text;
          if (!think_done) {
            // R1 思考段：缓冲中找闭合标记；闭合前丢弃（不投递下游）。
            // 跨 token 的 "</think>" 由累积缓冲天然拼接（累积用 rfind
            // 保守防思考内容里出现字面量的误判）。
            think_buf += text;
            const std::size_t pos = think_buf.rfind(kThinkEndTag);
            if (pos == std::string::npos) {
              continue;
            }
            // 闭合：之后的内容才是回答，投递（可能为空，等后续 token）。
            think_done = true;
            text = think_buf.substr(pos + kThinkEndTagLen);
            if (text.empty()) {
              continue;
            }
          }
          if (cb) {
            cb({BackendEvent::Kind::kToken, text, {}});
          }
        } else {
          // FINISH / ERROR：本次 run 终止（generate 统一补 kDone）。
          *accumulated += pending_waiting;
          if (!think_done && !think_buf.empty() && cb) {
            // 思考段未闭合（token 预算耗尽）：回退投递缓冲内容，保证
            // 下游有输出可读（与 StripThink 的未闭合回退语义一致）。
            cb({BackendEvent::Kind::kToken, think_buf, {}});
          }
          return true;
        }
      }
    }
  }

  LLMHandle handle = nullptr;
  EventCallback cb;                 // 只由 set_event_callback / generate 使用
  std::atomic<bool> cancelled{false};
  std::atomic<bool> running{false};
  std::mutex mu;                    // 保护 queue / generation / cb 交接
  std::condition_variable cv;
  std::deque<Item> queue;
  std::uint64_t generation = 0;
};

RkllmBackend::RkllmBackend(std::string model_path, int max_new_tokens,
                           int max_context_len) {
  impl_ = std::make_unique<Impl>(model_path, max_new_tokens, max_context_len);
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
  input.input_type = RKLLM_INPUT_PROMPT;
  input.prompt_input = prompt.c_str();

  RKLLMInferParam infer;
  std::memset(&infer, 0, sizeof(infer));
  infer.mode = RKLLM_INFER_GENERATE;
  infer.keep_history = 0;

  impl_->running.store(true);
  // rkllm_run 在 is_async=false 时同步阻塞（回调由调用线程触发，事件只能
  // 批量落地）；rkllm_run_async 立即返回，回调由厂商内部线程按 token
  // 流式触发，泵队列才能实时投递、生成中途才能取消。
  const int ret = rkllm_run_async(impl_->handle, &input, &infer, impl_.get());
  if (ret != 0) {
    // 本次生成未启动：不产出任何事件（与取消后空操作等价）。
    impl_->running.store(false);
    return;
  }
  std::string full;
  const bool normal_end = impl_->pump(gen, cb, &full);
  impl_->running.store(false);
  if (normal_end) {
    // kDone 携带正式回答：剥离 R1 思考段（思考过程不朗读，见 StripThink）。
    cb({BackendEvent::Kind::kDone, StripThink(std::move(full)), {}});
  }
}

void RkllmBackend::cancel() {
  impl_->cancelled.store(true);
  // 尽力而为中止厂商侧生成（返回码不可靠，token 过滤以 cancelled 为准）。
  if (impl_->running.load()) {
    rkllm_abort(impl_->handle);
  }
}

}  // namespace voxorchestra::backend::rkllm
