// RkllmOptions：RkllmBackend 的全部可配置项（纯值对象，不含厂商类型）。
//
// 用途：把原先硬编码在 rkllm_llm_backend.cpp 里的采样参数、思考模式开关、
// 思考段过滤标记和运行参数（CPU 绑核 / embed_flash / 域 id）全部外提为配置，
// 使同一份后端代码可以驱动不同模型的 .rkllm 产物（Qwen3 系列等），由
// config/*/session.json::llm 与 llm_node 命令行参数注入。
//
// 约束：本头文件不包含 rkllm.h，不依赖任何厂商 SDK，因此默认（无硬件）构建
// 也能编译并对 validate() 做单元测试。
#pragma once

#include <cmath>
#include <cstddef>
#include <string>

namespace voxorchestra::backend::rkllm {

struct RkllmOptions {
  // .rkllm 模型文件路径（必填）。
  std::string model_path;

  // 上下文窗口与单轮新增 token 上限。
  int max_new_tokens = 100;
  int max_context_len = 256;

  // 采样参数。top_k <= 0 表示交由供应商默认值决定；top_k = 1 为确定性贪心。
  int top_k = 1;
  float top_p = 0.95f;
  float temperature = 0.8f;
  float repeat_penalty = 1.1f;
  float frequency_penalty = 0.0f;
  float presence_penalty = 0.0f;

  // 跳过特殊 token；忽略 EOS（仅用于对照实验，正常部署保持 false）。
  bool skip_special_token = true;
  bool ignore_eos_token = false;

  // 思考模式开关（RKLLMInput.enable_thinking，Qwen3 系列生效）。
  // 关闭时模型不产出思考段；开启时配合 reasoning_end_tag 过滤。
  bool enable_thinking = false;

  // 思考段过滤标记：非空时，最后一次出现该标记之前（含标记本身）的内容不
  // 下发下游（TTS 不朗读思考过程）：会输出 <think>…</think> 的推理模型保持
  // "</think>"；对不产出思考段的模型（含当前默认模型）留空即完全关闭过滤。
  std::string reasoning_end_tag = "</think>";

  // 过滤缓冲上限（字节）：思考段迟迟不闭合时按原样放行，保证内存有界。
  std::size_t reasoning_max_buffer_bytes = 256u * 1024u;

  // 参与推理的 CPU 核数与掩码（bit0 = CPU0），沿用板端门禁基线 CPU0|CPU2。
  int enabled_cpus_num = 2;
  unsigned int enabled_cpus_mask = 0x01u | 0x04u;

  // 从闪存读取词嵌入向量；基座模型域 id（0 = 默认）。
  bool embed_flash = true;
  int base_domain_id = 0;
};

// 校验选项：返回空字符串表示合法，否则返回人类可读的原因。
// 节点在启动时调用（快速失败），避免把非法采样参数带进 rkllm_init。
inline std::string validate(const RkllmOptions& options) {
  if (options.model_path.empty()) {
    return "model_path 为空（--model 或 session.json::llm.model 必填）";
  }
  if (options.max_new_tokens <= 0) {
    return "max_new_tokens 必须为正整数";
  }
  if (options.max_context_len <= 0) {
    return "max_context_len 必须为正整数";
  }
  if (!(options.top_p > 0.0f) || options.top_p > 1.0f) {
    return "top_p 必须落在 (0, 1]";
  }
  if (!std::isfinite(options.temperature) || options.temperature < 0.0f) {
    return "temperature 必须为非负有限值";
  }
  if (!std::isfinite(options.repeat_penalty) ||
      options.repeat_penalty <= 0.0f) {
    return "repeat_penalty 必须为正有限值";
  }
  if (!std::isfinite(options.frequency_penalty) ||
      !std::isfinite(options.presence_penalty)) {
    return "frequency_penalty / presence_penalty 必须为有限值";
  }
  if (options.enabled_cpus_num < 1 || options.enabled_cpus_num > 8) {
    return "enabled_cpus_num 必须落在 [1, 8]";
  }
  if (options.enabled_cpus_mask == 0u || options.enabled_cpus_mask > 0xFFu) {
    return "enabled_cpus_mask 必须在 [0x01, 0xFF] 之内";
  }
  if (options.reasoning_max_buffer_bytes == 0u) {
    return "reasoning_max_buffer_bytes 必须为正";
  }
  return {};
}

}  // namespace voxorchestra::backend::rkllm
