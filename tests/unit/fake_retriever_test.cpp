// FakeRetriever 单元测试：Top-K 语义、排序确定性、注入知识库与无副作用。
#include "voxorchestra/backend/fake/fake_retriever.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

namespace eb = voxorchestra::backend;

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

// 默认知识库 Top-K：按得分降序返回前 K 条。
void test_top_k_default_knowledge() {
  eb::fake::FakeRetriever retriever;
  const auto top2 = retriever.retrieve("VoxOrchestra", 2);
  CHECK(top2.size() == 2);
  CHECK(top2[0].id == "k-voxorchestra");  // 0.95
  CHECK(top2[0].score == 0.95);
  CHECK(top2[1].id == "k-arch");      // 0.90
  CHECK(top2[1].score == 0.90);

  const auto all = retriever.retrieve("x", 99);
  CHECK(all.size() == 5);
  for (std::size_t i = 1; i < all.size(); ++i) {
    CHECK(all[i - 1].score >= all[i].score);  // 全程得分非增
  }
  std::cout << "  [ok] Top-K：得分降序截断（2 条 / 全量 5 条）" << std::endl;
}

// top_k 为 0：返回空列表；不崩溃。
void test_top_k_zero() {
  eb::fake::FakeRetriever retriever;
  CHECK(retriever.retrieve("x", 0).empty());
  std::cout << "  [ok] top_k=0 返回空列表" << std::endl;
}

// 注入知识库：同分条目按 id 升序（全序确定性）。
void test_injected_knowledge_tie_break() {
  std::vector<eb::RetrievedChunk> custom = {
      {"b", "条目 B", 0.5},
      {"a", "条目 A", 0.5},
      {"c", "条目 C", 0.9},
  };
  eb::fake::FakeRetriever retriever(std::move(custom));
  const auto result = retriever.retrieve("q", 3);
  CHECK(result.size() == 3);
  CHECK(result[0].id == "c");  // 0.9 最高
  CHECK(result[1].id == "a");  // 同分 0.5，id 升序在前
  CHECK(result[2].id == "b");
  std::cout << "  [ok] 注入知识库：得分降序、同分按 id 升序" << std::endl;
}

// 确定性：相同输入两次调用结果逐字段一致；retrieve 无副作用。
void test_determinism_and_no_side_effect() {
  eb::fake::FakeRetriever retriever;
  const auto a = retriever.retrieve("重复调用", 3);
  const auto b = retriever.retrieve("重复调用", 3);
  CHECK(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    CHECK(a[i].id == b[i].id);
    CHECK(a[i].text == b[i].text);
    CHECK(a[i].score == b[i].score);
  }
  // 第二次调用前不改变内部知识库：结果与首次完全一致。
  const auto c = retriever.retrieve("再次确认", 3);
  CHECK(c.size() == a.size());
  CHECK(c[0].id == a[0].id);
  std::cout << "  [ok] 确定性：同输入同输出、多次调用无副作用" << std::endl;
}

// Fake 语义文档化：得分与查询无关（验证检索链路而非真实相关性）。
void test_query_independent_scores() {
  eb::fake::FakeRetriever retriever;
  const auto a = retriever.retrieve("任意查询甲", 2);
  const auto b = retriever.retrieve("任意查询乙", 2);
  CHECK(a[0].id == b[0].id && a[1].id == b[1].id);
  std::cout << "  [ok] 得分与查询无关：Fake 只验证检索链路形状" << std::endl;
}

}  // namespace

int main() {
  std::cout << "fake_retriever_test:" << std::endl;
  test_top_k_default_knowledge();
  test_top_k_zero();
  test_injected_knowledge_tie_break();
  test_determinism_and_no_side_effect();
  test_query_independent_scores();

  if (g_failures == 0) {
    std::cout << "fake_retriever_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "fake_retriever_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
