// rkllm_smoke：RKLLM 版本链冒烟（init + 单次 run + 流式 callback 计时）。
//
// 目的：验证 librkllmrt.so + DeepSeek-R1-Distill-Qwen-1.5B_w4a16_RK3576.rkllm
//      + 板端 RKNPU driver 这条版本链能否 init 并推理，采集 TTFT / token·s。
// 来源：RKLLM 调用逻辑（Init / callback / run 参数）取自 LLM_Voice_Flow 的
//      llm_demo.cpp，剥离 ZMQ 全链路集成层，改为固定 prompt 直接推理。
// 编译：见同目录 build.sh（板端 aarch64 原生 g++，链接 librkllmrt.so）。
// rkllm.h 用了 int32_t / size_t 却未自带 <cstdint>/<cstddef>，必须在它之前引入。
#include <cstddef>
#include <cstdint>
#include "rkllm.h"

#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

static LLMHandle g_handle = nullptr;
static bool g_finished = false;
static bool g_error = false;
static int g_token_count = 0;
static bool g_first_seen = false;
static std::chrono::steady_clock::time_point g_run_start;
static std::chrono::steady_clock::time_point g_first_token;

static void smoke_callback(RKLLMResult *result, void * /*userdata*/, LLMCallState state) {
  if (state == RKLLM_RUN_NORMAL) {
    if (!g_first_seen) {
      g_first_seen = true;
      g_first_token = std::chrono::steady_clock::now();
    }
    ++g_token_count;
    if (result && result->text) {
      std::printf("%s", result->text);
      std::fflush(stdout);
    }
  } else if (state == RKLLM_RUN_FINISH) {
    g_finished = true;
    std::printf("\n");
  } else if (state == RKLLM_RUN_ERROR) {
    std::fprintf(stderr, "\n[RKLLM_RUN_ERROR]\n");
    g_error = true;
    g_finished = true;
  }
}

static void on_signal(int sig) {
  if (g_handle) {
    LLMHandle h = g_handle;
    g_handle = nullptr;
    rkllm_destroy(h);
  }
  std::_Exit(sig);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <model.rkllm> [prompt] [max_new_tokens]" << std::endl;
    return 1;
  }
  std::signal(SIGINT, on_signal);

  RKLLMParam param = rkllm_createDefaultParam();
  param.model_path = argv[1];
  param.top_k = 1;
  param.top_p = 0.95f;
  param.temperature = 0.8f;
  param.repeat_penalty = 1.1f;
  param.frequency_penalty = 0.0f;
  param.presence_penalty = 0.0f;
  param.max_new_tokens = (argc >= 4) ? std::atoi(argv[3]) : 64;
  param.max_context_len = 256;
  param.skip_special_token = true;
  param.extend_param.base_domain_id = 0;
  param.extend_param.embed_flash = 1;
  param.extend_param.enabled_cpus_num = 2;
  param.extend_param.enabled_cpus_mask = CPU0 | CPU2;

  std::cout << "rkllm init start" << std::endl;
  int ret = rkllm_init(&g_handle, &param, smoke_callback);
  if (ret != 0 || g_handle == nullptr) {
    std::cerr << "rkllm init failed ret=" << ret << std::endl;
    return 2;
  }
  std::cout << "rkllm init success" << std::endl;

  // DeepSeek-R1-Distill 的对话模板（与 llm_demo.cpp 一致，全角｜定界符）。
  // 全角｜= U+FF5C = UTF-8 EF BD 9C；用字面量拼接断开，避免 \x9C 后跟 hex
  // 字符（A-F/a-f/0-9）被贪婪解析成越界的 \x9CA 等。
  rkllm_set_chat_template(g_handle, "", "\xEF\xBD\x9C" "User" "\xEF\xBD\x9C",
                          "\xEF\xBD\x9C" "Assistant" "\xEF\xBD\x9C");

  const std::string prompt = (argc >= 3) ? argv[2] : std::string("你好，请用一句话介绍你自己。");
  RKLLMInput input;
  std::memset(&input, 0, sizeof(input));
  input.input_type = RKLLM_INPUT_PROMPT;
  input.prompt_input = const_cast<char *>(prompt.c_str());

  RKLLMInferParam infer;
  std::memset(&infer, 0, sizeof(infer));
  infer.mode = RKLLM_INFER_GENERATE;
  infer.keep_history = 0;

  std::cout << "prompt: " << prompt << std::endl;
  std::cout << "max_new_tokens: " << param.max_new_tokens << std::endl;
  std::cout << "=== run start ===" << std::endl;
  g_run_start = std::chrono::steady_clock::now();
  ret = rkllm_run(g_handle, &input, &infer, nullptr);
  if (ret != 0) {
    std::cerr << "rkllm_run returned non-zero ret=" << ret << std::endl;
  }

  // rkllm_run 异步产出 token；等 callback FINISH。
  while (!g_finished) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const auto done = std::chrono::steady_clock::now();

  const double ttft_ms =
      g_first_seen ? std::chrono::duration<double, std::milli>(g_first_token - g_run_start).count()
                   : -1.0;
  const double total_s = std::chrono::duration<double>(done - g_run_start).count();
  const double decode_s =
      g_first_seen ? std::chrono::duration<double>(done - g_first_token).count() : 0.0;
  const int decode_tokens = g_first_seen ? (g_token_count - 1) : 0;

  std::cout << "=== metrics ===" << std::endl;
  std::cout << "tokens_total: " << g_token_count << std::endl;
  std::cout << "TTFT_ms: " << ttft_ms << std::endl;
  std::cout << "total_s: " << total_s << std::endl;
  std::cout << "decode_tokens: " << decode_tokens
            << "  decode_tok_per_s: " << (decode_s > 0 ? decode_tokens / decode_s : 0.0)
            << std::endl;

  rkllm_destroy(g_handle);
  g_handle = nullptr;
  return g_error ? 3 : 0;
}
