// RkllmBackend：RKLLM Runtime 真实大模型文本生成后端。
// Author: Caden
//
// 当前模型的使用与行为要点：
//   - rkllm_createDefaultParam → 采样/运行参数 → rkllm_init → rkllm_run_async
//     → callback NORMAL/WAITING/FINISH/ERROR → rkllm_destroy；
//   - 事件流与 FakeLlmBackend 协议等价：generate(prompt) 逐 token 产出
//     kToken（每事件一个厂商 callback 文本段），结束后 kDone 携带完整输出文本；
//   - cancel() 后不再产出任何事件（含已入队未投递的旧 token，过滤点在
//     generate 泵队列处），generate 变为空操作；
//   - set_event_callback 开启新会话：清空残留队列并重置取消状态。
// 线程纪律：RKLLM 回调来自厂商内部线程，回调只做"速拷入受控队列"，
// BackendEvent 一律由 generate 的调用线程泵队列时投递；厂商侧取消
// （rkllm_abort）返回值不可靠，仅尽力而为，token 过滤以本地 cancelled
// 标志为准（generation 标签丢弃上一会话残留）。
//
// 模型无关性：采样参数、思考模式开关、思考段过滤标记、CPU 绑核等全部由
// RkllmOptions 注入，本文件不再包含任何针对单一模型的硬编码假设；对话模板
// 始终使用 .rkllm 产物自带的那一份。
//
// 上游源码不随仓库分发（third_party/README.md 约定），构建时由
// SLOTNEXUS_RKLLM_ROOT 指向板端 SDK 目录（include/rkllm.h +
// aarch64/librkllmrt.so）；模型文件不入库，由配置传入
// （config/taishanpi3m/session.json::llm）。
#pragma once

#include <memory>
#include <string>

#include "slotnexus/backend/i_llm_backend.hpp"
#include "slotnexus/backend/rkllm/rkllm_options.hpp"

namespace slotnexus::backend::rkllm {

class RkllmBackend final : public ILlmBackend {
 public:
  // options.model_path：.rkllm 模型文件（当前为 Qwen3.5-0.8B W4A16 RK3576，
  // 由部署配置传入）。
  // 其余字段见 rkllm_options.hpp；非法选项由调用方用 validate() 提前拦截，
  // 这里仍会在 rkllm_init 失败时抛出 std::runtime_error。
  explicit RkllmBackend(RkllmOptions options);
  ~RkllmBackend() override;

  RkllmBackend(const RkllmBackend&) = delete;
  RkllmBackend& operator=(const RkllmBackend&) = delete;

  void set_event_callback(EventCallback cb) override;
  void generate(const std::string& prompt) override;
  void cancel() override;

 private:
  struct Impl;  // 上游类型（LLMHandle / RKLLMParam / 回调）只存在于 .cpp
  std::unique_ptr<Impl> impl_;
};

}  // namespace slotnexus::backend::rkllm
