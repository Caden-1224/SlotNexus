// SentenceChunker 单元测试：中英文句末标点、连续标点、换行、流式切分与收尾。
// Author: Caden
#include "slotnexus/common/sentence_chunker.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace cq = slotnexus::common;

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

// 便捷断言：分句结果逐条相等。
void check_sentences(const std::vector<std::string>& got,
                     const std::vector<std::string>& want) {
  CHECK(got.size() == want.size());
  for (std::size_t i = 0; i < got.size() && i < want.size(); ++i) {
    if (got[i] != want[i]) {
      ++g_failures;
      std::cerr << "FAIL 分句 [" << i << "] 期望 [" << want[i] << "] 实际 ["
                << got[i] << "]" << std::endl;
    }
  }
}

// 返回 count 个 UTF-8 单元拼接的字符串（测试中用于构造固定长度的中文文本）。
std::string repeat_utf8(const std::string& unit, std::size_t count) {
  std::string out;
  out.reserve(unit.size() * count);
  for (std::size_t i = 0; i < count; ++i) {
    out += unit;
  }
  return out;
}

void test_cjk_terminators() {
  cq::SentenceChunker c;
  // 中文句号/感叹号/问号/分号；无标点的收尾句在 flush 返回。
  check_sentences(c.feed("第一句。第二句！第三句？第四句；第五句"),
                  {"第一句。", "第二句！", "第三句？", "第四句；"});
  check_sentences(c.flush(), {"第五句"});
  std::cout << "  [ok] 中文句末标点：。！？； 均正确切分且归属前句" << std::endl;
}

void test_ascii_terminators() {
  cq::SentenceChunker c;
  check_sentences(c.feed("One!Two?Three;Four"), {"One!", "Two?", "Three;"});
  check_sentences(c.flush(), {"Four"});
  std::cout << "  [ok] 英文句末标点：!?; 正确切分" << std::endl;
}

void test_consecutive_terminators() {
  cq::SentenceChunker c;
  // 连续标点只保留首个。
  check_sentences(c.feed("第一句！！第二句??第三句"), {"第一句！", "第二句?"});
  check_sentences(c.flush(), {"第三句"});
  std::cout << "  [ok] 连续句末标点：只保留首个" << std::endl;
}

void test_newline_splits() {
  cq::SentenceChunker c;
  check_sentences(c.feed("第一行\n第二行\r\n第三行"), {"第一行", "第二行"});
  check_sentences(c.flush(), {"第三行"});
  std::cout << "  [ok] 换行切分：\\n 与 \\r\\n，换行符不进入句子" << std::endl;
}

void test_streaming_feed() {
  cq::SentenceChunker c;
  // 流式：句子跨多次 feed。
  check_sentences(c.feed("第一句。第二句"), {"第一句。"});
  check_sentences(c.feed("未完成"), {});
  check_sentences(c.feed("，补全。第三句"), {"第二句未完成，补全。"});
  check_sentences(c.flush(), {"第三句"});
  std::cout << "  [ok] 流式切分：句子可跨多次 feed 累积" << std::endl;
}

void test_flush_remainder() {
  cq::SentenceChunker c;
  check_sentences(c.feed("没有标点的句子"), {});
  check_sentences(c.flush(), {"没有标点的句子"});
  // flush 后状态重置。
  check_sentences(c.feed("重新开始。"), {"重新开始。"});
  check_sentences(c.flush(), {});
  std::cout << "  [ok] flush：未完成句子收尾返回，状态重置" << std::endl;
}

void test_commas_do_not_split() {
  cq::SentenceChunker c;
  // 逗号、顿号不切分。
  check_sentences(c.feed("你好，世界。万物，皆有。"), {"你好，世界。",
                                                  "万物，皆有。"});
  check_sentences(c.flush(), {});
  std::cout << "  [ok] 逗号不切分：整句完整保留" << std::endl;
}

void test_max_bytes_limit() {
  cq::SentenceChunker c(6);
  // 两字汉字 = 6 字节，达到上限即交付，不等到句末标点。
  check_sentences(c.feed("你好世界再见"), {"你好", "世界"});
  check_sentences(c.flush(), {"再见"});

  // 标点优先归属前句；为了保留标点，单片可超过上限一个 UTF-8 字符。
  cq::SentenceChunker c2(6);
  check_sentences(c2.feed("你好世界。"), {"你好", "世界。"});
  check_sentences(c2.flush(), {});

  // max_bytes = 0 保持只按标点切分的旧行为。
  cq::SentenceChunker c3(0);
  check_sentences(c3.feed("你好世界"), {});
  check_sentences(c3.flush(), {"你好世界"});
  std::cout << "  [ok] 容量上限：按 UTF-8 边界切长句，标点仍归属前句"
            << std::endl;
}

void test_first_chunk_limit() {
  // 首片无标点时按 42 字节（14 个汉字）软上限切，随后切回普通 60 字节。
  cq::SentenceChunker c(60, 42);
  const std::string first = repeat_utf8("甲", 14);
  const std::string tail = repeat_utf8("乙", 21);
  check_sentences(c.feed(first), {first});  // 恰好 42 字节，达到首片软上限即切
  check_sentences(c.feed(tail),
                  {repeat_utf8("乙", 20)});  // 后续恢复 60 字节普通上限
  check_sentences(c.flush(), {repeat_utf8("乙", 1)});
  std::cout << "  [ok] 首片软上限：无标点达到 14 字即切，后续恢复普通上限"
            << std::endl;
}

void test_first_chunk_zero_disables_early_cut() {
  // first_max_bytes=0 表示不启用首片提前切分，仍按普通 max_bytes 原触发方式。
  cq::SentenceChunker c(60, 0);
  const std::string text = repeat_utf8("甲", 20);  // 60 字节，等下一个字符触发
  check_sentences(c.feed(text), {});
  check_sentences(c.flush(), {text});
  std::cout << "  [ok] 首片软上限 0：不提前切分，保持普通上限行为" << std::endl;
}

void test_first_chunk_punctuation_priority() {
  cq::SentenceChunker c(60, 42);
  // 标点优先：即使未到首片软上限，也按自然句末切分。
  check_sentences(c.feed("短句。"), {"短句。"});
  // 首片已由标点发出，之后恢复 60 字节普通上限。
  const std::string tail = repeat_utf8("甲", 20);
  check_sentences(c.feed(tail), {});
  check_sentences(c.feed("乙"), {tail});
  check_sentences(c.flush(), {"乙"});
  std::cout << "  [ok] 首片标点优先：自然句末先切，后续恢复普通上限"
            << std::endl;
}

void test_first_chunk_flush_resets() {
  cq::SentenceChunker c(60, 42);
  const std::string first = repeat_utf8("甲", 14);
  check_sentences(c.feed(first), {first});
  check_sentences(c.flush(), {});
  // flush 后首片状态重置：新一轮仍在达到首片软上限时立即切分。
  check_sentences(c.feed(first), {first});
  check_sentences(c.flush(), {});
  std::cout << "  [ok] flush：收尾后重置首片状态" << std::endl;
}

void test_utf8_boundaries_and_old_behavior() {
  // 容量切分不跨 UTF-8 字符：6 字节首片上限容纳两个汉字。
  cq::SentenceChunker c(6, 6);
  check_sentences(c.feed("你好世界"), {"你好"});
  check_sentences(c.flush(), {"世界"});

  // feed 间按字节边界输入：残留字节补齐后才处理，跨 feed 的中文标点仍有效。
  cq::SentenceChunker byte_stream(0, 42);
  const std::string text = "第一句。第二句";
  std::vector<std::string> got;
  for (char ch : text) {
    for (auto& piece : byte_stream.feed(std::string(1, ch))) {
      got.push_back(std::move(piece));
    }
  }
  for (auto& piece : byte_stream.flush()) {
    got.push_back(std::move(piece));
  }
  check_sentences(got, {"第一句。", "第二句"});

  // max_bytes=0 保持只按标点切分，首片软上限被忽略。
  cq::SentenceChunker only_punct(0, 42);
  const std::string long_text = repeat_utf8("甲", 30);
  check_sentences(only_punct.feed(long_text), {});
  check_sentences(only_punct.flush(), {long_text});
  std::cout << "  [ok] UTF-8 边界：跨 feed 字符完整，0 上限保持旧行为"
            << std::endl;
}

void test_edge_cases() {
  cq::SentenceChunker c;
  // 空输入。
  check_sentences(c.feed(""), {});
  check_sentences(c.flush(), {});
  // 只有标点：无内容句子不产出。
  check_sentences(c.feed("？？"), {});
  check_sentences(c.flush(), {});
  // 句子不以标点开头：句首标点丢弃。
  cq::SentenceChunker c2;
  check_sentences(c2.feed("。开头"), {});
  check_sentences(c2.flush(), {"开头"});
  std::cout << "  [ok] 边界：空输入、纯标点、开头标点均不崩溃且确定"
            << std::endl;
}

}  // namespace

int main() {
  std::cout << "sentence_chunker_test:" << std::endl;
  test_cjk_terminators();
  test_ascii_terminators();
  test_consecutive_terminators();
  test_newline_splits();
  test_streaming_feed();
  test_flush_remainder();
  test_commas_do_not_split();
  test_max_bytes_limit();
  test_first_chunk_limit();
  test_first_chunk_zero_disables_early_cut();
  test_first_chunk_punctuation_priority();
  test_first_chunk_flush_resets();
  test_utf8_boundaries_and_old_behavior();
  test_edge_cases();

  if (g_failures == 0) {
    std::cout << "sentence_chunker_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "sentence_chunker_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
