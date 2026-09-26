// RkllmBackend 实现：封装 RKLLM Runtime C API（librkllmrt.so）。
//
// 调用序列与 rkllm_smoke.cpp 一致（行为依据）：
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

#include "voxorchestra/backend/rkllm/rkllm_llm_backend.hpp"
#include "voxorchestra/backend/rkllm/rkllm_reasoning_filter.hpp"

namespace voxorchestra::backend::rkllm {

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
#if defined(VOXORCHESTRA_RKLLM_INIT_CALLBACK_STRUCT)
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

#if defined(VOXORCHESTRA_RKLLM_INIT_CALLBACK_STRUCT)
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
            std::string* delivered) {
    delivered->clear();
    std::string pending_waiting;  // WAITING 状态携带的 UTF-8 半字符
    // 思考段过滤（每轮 generate 独立状态）：见 ReasoningFilter 头文件注释。
    ReasoningFilter filter(options_.reasoning_end_tag,
                           options_.reasoning_max_buffer_bytes);

    // 把过滤结果投递给下游；空结果不下发。
    const auto emit = [&](const std::string& raw) {
      std::string out = filter.accept(raw);
      if (out.empty()) {
        return;
      }
      *delivered += out;
      if (cb) {
        cb({BackendEvent::Kind::kToken, out, {}});
      }
    };

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
          emit(text);
        } else {
          // FINISH / ERROR：本次 run 终止（generate 统一补 kDone）。
          if (!pending_waiting.empty()) {
            emit(pending_waiting);
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
            *delivered += tail;
            if (cb) {
              cb({BackendEvent::Kind::kToken, tail, {}});
            }
          }
          return true;
        }
      }
    }
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
  const bool normal_end = impl_->pump(gen, cb, &delivered);
  impl_->running.store(false);
  if (normal_end) {
    // kDone 携带实际下发文本（思考段已在 pump 内剔除）。
    cb({BackendEvent::Kind::kDone, std::move(delivered), {}});
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
