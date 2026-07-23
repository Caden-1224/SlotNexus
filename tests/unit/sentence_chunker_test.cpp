// SentenceChunker 单元测试：中英文句末标点、连续标点、换行、流式切分与收尾。
#include "voxorchestra/common/sentence_chunker.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace cq = voxorchestra::common;

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
  test_edge_cases();

  if (g_failures == 0) {
    std::cout << "sentence_chunker_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "sentence_chunker_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
