// RkllmBackend：RKLLM Runtime 真实大模型文本生成后端。
//
// 行为依据：artifacts/upstream-baseline/rkllm_smoke.cpp（板端门禁已过，
// rkllm_init success / 7.79 tok/s / TTFT 288 ms）与 llm_demo.cpp：
//   - rkllm_createDefaultParam → 采样参数 → rkllm_set_chat_template（全角
//     ｜User｜/｜Assistant｜）→ rkllm_init → rkllm_run（异步）→
//     callback FINISH/ERROR → rkllm_destroy；
//   - 事件流与 FakeLlmBackend 协议等价：generate(prompt) 逐 token 产出
//     kToken（每事件一个厂商 callback NORMAL 文本段），结束后 kDone 携带
//     完整输出文本；
//   - cancel() 后不再产出任何事件（含已入队未投递的旧 token，过滤点在
//     generate 泵队列处），generate 变为空操作；
//   - set_event_callback 开启新会话：清空残留队列并重置取消状态。
// 线程纪律：RKLLM 回调来自厂商内部线程，回调只做"速拷入受控队列"，
// BackendEvent 一律由 generate 的调用线程泵队列时投递；厂商侧取消
// （rkllm_abort）返回值不可靠，仅尽力而为，token 过滤以本地 cancelled
// 标志为准（generation 标签丢弃上一会话残留）。
//
// 上游源码不随仓库分发（third_party/README.md 约定），构建时由
// VOXORCHESTRA_RKLLM_ROOT 指向板端 SDK 目录（include/rkllm.h +
// aarch64/librkllmrt.so）；模型文件不入库，由配置传入
// （config/taishanpi3m/session.json::llm）。
#pragma once

#include <memory>
#include <string>

#include "voxorchestra/backend/i_llm_backend.hpp"

namespace voxorchestra::backend::rkllm {

class RkllmBackend final : public ILlmBackend {
 public:
  // model_path：.rkllm 模型文件（DeepSeek-R1-Distill-Qwen-1.5B W4A16 RK3576，
  // 哈希见 artifacts/upstream-baseline/）。
  // 初始化（rkllm_init）失败抛出 std::runtime_error。
  // max_new_tokens / max_context_len：采样参数（教程参考值 100 / 256，
  // 板端实测校准，见 artifacts/llm-integration/）。
  RkllmBackend(std::string model_path, int max_new_tokens = 100,
               int max_context_len = 256);
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

}  // namespace voxorchestra::backend::rkllm
