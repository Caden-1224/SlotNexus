// RKLLM 纯逻辑单元测试：思考段过滤 + 选项校验。
//
// 这两块逻辑原先硬编码在 rkllm_llm_backend.cpp 里（单一模型专用），
// 改造后外提为不含厂商 SDK 的头文件，因此默认（无硬件）构建即可验证：
//   - 思考段标记跨 token 断开时要能正确闭合；
//   - 标记重复出现时取最后一次（保守，避免思考内容里的字面量造成误切）；
//   - 思考段始终不闭合（token 预算耗尽）时要回退放行，不能整轮无输出；
//   - 缓冲上限触发后必须放行且内存有界；
//   - 非法采样/运行参数在启动阶段被 validate() 拦住。
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

#include "slotnexus/backend/rkllm/rkllm_options.hpp"
#include "slotnexus/backend/rkllm/rkllm_reasoning_filter.hpp"

namespace er = slotnexus::backend::rkllm;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      ++g_failures;                                                        \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #cond \
                << std::endl;                                              \
    }                                                                      \
  } while (0)

#define CHECK_EQ(actual, expected)                                            \
  do {                                                                        \
    const std::string a_ = (actual);                                          \
    const std::string e_ = (expected);                                        \
    if (a_ != e_) {                                                           \
      ++g_failures;                                                           \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #actual  \
                << " = [" << a_ << "]，期望 [" << e_ << "]" << std::endl;     \
    }                                                                         \
  } while (0)

// end_tag 为空：完全关闭过滤，直通且零状态。
void test_disabled_is_passthrough() {
  er::ReasoningFilter f("");
  CHECK(!f.enabled());
  CHECK_EQ(f.accept("<think>思考</think>回答"), "<think>思考</think>回答");
  CHECK_EQ(f.accept("后续"), "后续");
  CHECK_EQ(f.flush(), "");
  std::cout << "  [ok] end_tag 为空：直通、无缓冲" << std::endl;
}

// 标记与内容同在一个增量里：只下发标记之后的内容。
void test_single_chunk_closes() {
  er::ReasoningFilter f("</think>");
  CHECK(f.enabled());
  CHECK_EQ(f.accept("<think>内部推理</think>正式回答"), "正式回答");
  CHECK(f.closed());
  CHECK_EQ(f.flush(), "");
  std::cout << "  [ok] 单块闭合：只下发标记之后的内容" << std::endl;
}

// 标记跨 token 断开：闭合前不下发任何增量，闭合后立即下发。
void test_tag_split_across_tokens() {
  er::ReasoningFilter f("</think>");
  CHECK_EQ(f.accept("<thi"), "");
  CHECK_EQ(f.accept("nk>思"), "");
  CHECK_EQ(f.accept("考</thi"), "");
  CHECK(!f.closed());
  CHECK_EQ(f.accept("nk>你好"), "你好");
  CHECK(f.closed());
  CHECK(f.buffered_bytes() == 0u);
  std::cout << "  [ok] 跨 token 断开：闭合前不发、闭合后立即发" << std::endl;
}

// 标记重复出现：取最后一次（保守），前面出现过的字面量不会提前放行。
void test_last_occurrence_wins() {
  er::ReasoningFilter f("</think>");
  CHECK_EQ(f.accept("思考里提到</think>这个词</think>真正回答"), "真正回答");
  std::cout << "  [ok] 标记重复：取最后一次出现" << std::endl;
}

// 会话结束时仍未闭合：flush 回退放行，保证下游有内容可读。
void test_unclosed_falls_back_on_flush() {
  er::ReasoningFilter f("</think>");
  CHECK_EQ(f.accept("<think>预算耗尽只写了思考"), "");
  CHECK(!f.closed());
  CHECK_EQ(f.flush(), "<think>预算耗尽只写了思考");
  CHECK(f.closed());
  CHECK_EQ(f.flush(), "");  // 幂等
  std::cout << "  [ok] 未闭合：flush 回退放行且幂等" << std::endl;
}

// 缓冲超上限：放行并关闭过滤，内存不随未闭合思考段无限增长。
void test_buffer_limit_releases() {
  er::ReasoningFilter f("</think>", 8u);
  CHECK_EQ(f.accept("1234567890"), "1234567890");
  CHECK(f.closed());
  CHECK(f.buffered_bytes() == 0u);
  CHECK_EQ(f.accept("后续"), "后续");
  std::cout << "  [ok] 超上限：原样放行、关闭过滤、缓冲清空" << std::endl;
}

// 闭合标记是最后一个增量：先返回空，之后的新增量直通。
void test_empty_tail_then_passthrough() {
  er::ReasoningFilter f("</think>");
  CHECK_EQ(f.accept("推理</think>"), "");
  CHECK(f.closed());
  CHECK_EQ(f.accept("第一条正式内容"), "第一条正式内容");
  std::cout << "  [ok] 标记收尾：空尾部不发、后续直通" << std::endl;
}

// 选项校验：默认值合法；越界字段逐个被拦下。
void test_options_validation() {
  er::RkllmOptions ok;
  ok.model_path = "/tmp/model.rkllm";
  CHECK(er::validate(ok).empty());

  er::RkllmOptions no_model = ok;
  no_model.model_path.clear();
  CHECK(!er::validate(no_model).empty());

  er::RkllmOptions bad_tokens = ok;
  bad_tokens.max_new_tokens = 0;
  CHECK(!er::validate(bad_tokens).empty());

  er::RkllmOptions bad_context = ok;
  bad_context.max_context_len = 0;
  CHECK(!er::validate(bad_context).empty());

  er::RkllmOptions bad_top_p = ok;
  bad_top_p.top_p = 0.0f;
  CHECK(!er::validate(bad_top_p).empty());
  bad_top_p.top_p = 1.5f;
  CHECK(!er::validate(bad_top_p).empty());

  er::RkllmOptions bad_temp = ok;
  bad_temp.temperature = std::numeric_limits<float>::quiet_NaN();
  CHECK(!er::validate(bad_temp).empty());
  bad_temp.temperature = -0.1f;
  CHECK(!er::validate(bad_temp).empty());

  er::RkllmOptions bad_penalty = ok;
  bad_penalty.repeat_penalty = 0.0f;
  CHECK(!er::validate(bad_penalty).empty());

  er::RkllmOptions bad_cpus = ok;
  bad_cpus.enabled_cpus_num = 0;
  CHECK(!er::validate(bad_cpus).empty());
  bad_cpus.enabled_cpus_num = 9;
  CHECK(!er::validate(bad_cpus).empty());

  er::RkllmOptions bad_mask = ok;
  bad_mask.enabled_cpus_mask = 0u;
  CHECK(!er::validate(bad_mask).empty());
  bad_mask.enabled_cpus_mask = 0x1FFu;
  CHECK(!er::validate(bad_mask).empty());

  er::RkllmOptions bad_buffer = ok;
  bad_buffer.reasoning_max_buffer_bytes = 0u;
  CHECK(!er::validate(bad_buffer).empty());

  er::RkllmOptions thinking = ok;
  thinking.enable_thinking = true;      // 思考模式本身合法
  thinking.reasoning_end_tag.clear();   // 关闭过滤也合法
  CHECK(er::validate(thinking).empty());

  std::cout << "  [ok] 选项校验：默认值通过，越界字段逐个拦截" << std::endl;
}

}  // namespace

int main() {
  std::cout << "rkllm_reasoning_filter_test:" << std::endl;
  test_disabled_is_passthrough();
  test_single_chunk_closes();
  test_tag_split_across_tokens();
  test_last_occurrence_wins();
  test_unclosed_falls_back_on_flush();
  test_buffer_limit_releases();
  test_empty_tail_then_passthrough();
  test_options_validation();

  if (g_failures == 0) {
    std::cout << "rkllm_reasoning_filter_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "rkllm_reasoning_filter_test 失败 " << g_failures << " 项"
            << std::endl;
  return 1;
}
