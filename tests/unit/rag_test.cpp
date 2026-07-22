// RAG 单元测试：文本规范化、BM25 打分（与手算公式对照）、JSONL 知识库、
// L0-L3 分级路由（阈值可配、空知识库、控制关键词绕过检索）。
#include "voxorchestra/rag/bm25.hpp"
#include "voxorchestra/rag/knowledge_store.hpp"
#include "voxorchestra/rag/router.hpp"
#include "voxorchestra/rag/text_normalizer.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>  // getpid（临时文件唯一名）

namespace rg = voxorchestra::rag;
using rg::RouteLevel;

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

#define CHECK_NEAR(a, b, eps)                                                \
  do {                                                                       \
    const double va = (a), vb = (b);                                         \
    if (std::fabs(va - vb) > (eps)) {                                        \
      ++g_failures;                                                          \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #a      \
                << "=" << va << " != " << #b << "=" << vb << std::endl;      \
    }                                                                        \
  } while (0)

// ---------- 文本规范化与分词 ----------

void test_normalize_and_tokenize() {
  // CJK 逐字、ASCII 单词整体、大小写折叠。
  CHECK(rg::tokenize("你好世界") == std::vector<std::string>(
                                         {"你", "好", "世", "界"}));
  CHECK(rg::tokenize("VoxOrchestra 是什么") ==
        std::vector<std::string>({"voxorchestra", "是", "什", "么"}));
  // 全角折叠：全角字母折半角并小写。
  CHECK(rg::normalize_text("ＡＢＣ") == "abc");
  // 标点不是 token，但分隔相邻词。
  CHECK(rg::tokenize("cat,dog!") == std::vector<std::string>({"cat", "dog"}));
  // 空白折叠为单个空格。
  CHECK(rg::normalize_text("a  b\n\tc") == "a b c");
  // 空串。
  CHECK(rg::tokenize("").empty());
  CHECK(rg::normalize_text("  ").empty());
  std::cout << "  [ok] 规范化与分词：CJK 逐字、ASCII 词、全角折叠、标点丢弃"
            << std::endl;
}

// ---------- BM25 打分 ----------

// 构造小型英文语料：d0 "a b"，d1 "a c"，d2 "b c"。
// N=3、avgdl=1；"a"/"b"/"c" 各出现于 2 篇文档，
// IDF = ln(1 + (3-2+0.5)/(2+0.5)) = ln(1.6)。
// d0 对查询 "a"：f=1、|d|=1、norm = k1*(1-b+b*1/1) = k1 = 1.5，
// score = IDF * 2.5/(1+1.5) = IDF。
rg::Bm25Index make_tiny_index() {
  rg::Bm25Index index;
  index.add_document("a b");  // d0
  index.add_document("a c");  // d1
  index.add_document("b c");  // d2
  index.build();
  return index;
}

void test_bm25_hand_formula() {
  const rg::Bm25Index index = make_tiny_index();
  // IDF 与手算公式一致（直接重推导公式，非循环引用）。
  const double expected_idf = std::log(1.6);
  CHECK_NEAR(index.idf("a"), expected_idf, 1e-12);
  CHECK_NEAR(index.idf("b"), expected_idf, 1e-12);
  // 单 token 查询 "a" 对 d0：score = IDF * (k1+1)/(f + k1*(1-b+b*|d|/avgdl))，
  // 这里 f=1、|d|=avgdl=1 → score = IDF。
  CHECK_NEAR(index.score("a", 0), expected_idf, 1e-12);
  // 双 token 查询 "a b" 对 d0：两个 token 各贡献 IDF。
  CHECK_NEAR(index.score("a b", 0), 2.0 * expected_idf, 1e-12);
  // 不含查询词的文档得 0 分。
  CHECK(index.score("a", 2) == 0.0);
  std::cout << "  [ok] BM25：IDF 与单/双 token 得分与手算公式一致" << std::endl;
}

void test_bm25_rank_and_determinism() {
  const rg::Bm25Index index = make_tiny_index();
  // 查询 "b"：d0、d2 命中且同分，d1 为 0；排名按 doc_index 升序（全序）。
  const auto hits = index.rank("b");
  CHECK(hits.size() == 2);
  CHECK(hits[0].doc_index == 0 && hits[1].doc_index == 2);
  CHECK_NEAR(hits[0].score, hits[1].score, 1e-12);
  // 重复查询结果逐字段一致（确定性）。
  const auto again = index.rank("b");
  CHECK(again.size() == hits.size());
  for (std::size_t i = 0; i < hits.size(); ++i) {
    CHECK(again[i].doc_index == hits[i].doc_index);
    CHECK(again[i].score == hits[i].score);
  }
  // Top-K 截断；k=0 返回空。
  CHECK(index.top_k("b", 1).size() == 1);
  CHECK(index.top_k("b", 0).empty());
  // 空查询不命中。
  CHECK(index.rank("").empty());
  // 未 build 的索引：不崩溃、得 0 分。
  rg::Bm25Index fresh;
  fresh.add_document("x y");
  CHECK(fresh.score("x", 0) == 0.0);
  std::cout << "  [ok] BM25：排名全序、Top-K 截断、确定性、未 build 安全"
            << std::endl;
}

// ---------- JSONL 知识库 ----------

// 测试用临时 JSONL 文件路径（当前工作目录，测试结束删除）。
std::string tmp_kb_path(const std::string& name) {
  return std::filesystem::temp_directory_path().string() + "/vox_rag_" + name +
         "_" + std::to_string(::getpid()) + ".jsonl";
}

void test_knowledge_store_load() {
  const std::string path = tmp_kb_path("load");
  {
    std::ofstream out(path);
    out << "# 注释行\n"
        << "\n"  // 空行
        << "{\"id\": \"k-1\", \"text\": \"第一条\"}\n"
        << "{\"text\": \"第二条\"}\n";  // id 缺省 → k2
  }
  const rg::KnowledgeStore store(path);
  CHECK(store.size() == 2);
  CHECK(store.entries()[0].id == "k-1");
  CHECK(store.entries()[0].text == "第一条");
  CHECK(store.entries()[1].id == "k2");  // 缺省 id 按行号
  CHECK(store.entries()[1].text == "第二条");
  std::remove(path.c_str());
  std::cout << "  [ok] JSONL 知识库：注释/空行忽略、缺省 id 按行号" << std::endl;
}

void test_knowledge_store_errors() {
  bool threw = false;
  try {
    const rg::KnowledgeStore store(tmp_kb_path("missing"));  // 文件不存在
  } catch (const rg::KnowledgeStoreError& e) {
    threw = true;
    CHECK(std::string(e.what()).find("无法读取") != std::string::npos);
  }
  CHECK(threw);

  const std::string bad_json = tmp_kb_path("badjson");
  { std::ofstream out(bad_json); out << "{\"text\": 不是json\n"; }
  threw = false;
  try {
    const rg::KnowledgeStore store(bad_json);
  } catch (const rg::KnowledgeStoreError& e) {
    threw = true;
    CHECK(std::string(e.what()).find("JSON 解析失败") != std::string::npos);
  }
  CHECK(threw);

  const std::string no_text = tmp_kb_path("notext");
  { std::ofstream out(no_text); out << "{\"id\": \"k-1\", \"other\": 1}\n"; }
  threw = false;
  try {
    const rg::KnowledgeStore store(no_text);
  } catch (const rg::KnowledgeStoreError& e) {
    threw = true;
    CHECK(std::string(e.what()).find("text") != std::string::npos);
  }
  CHECK(threw);

  const std::string dup_id = tmp_kb_path("dupid");
  { std::ofstream out(dup_id); out << "{\"id\": \"k\", \"text\": \"a\"}\n{\"id\": \"k\", \"text\": \"b\"}\n"; }
  threw = false;
  try {
    const rg::KnowledgeStore store(dup_id);
  } catch (const rg::KnowledgeStoreError& e) {
    threw = true;
    CHECK(std::string(e.what()).find("重复") != std::string::npos);
  }
  CHECK(threw);

  std::remove(bad_json.c_str());
  std::remove(no_text.c_str());
  std::remove(dup_id.c_str());
  std::cout << "  [ok] JSONL 知识库：文件缺失/非法 JSON/缺 text/重复 id 均报错"
            << std::endl;
}

// ---------- L0-L3 路由 ----------

// 与 BM25 测试相同的语料，但带 id/text（Router 需要条目内容）。
struct RouterFixture {
  rg::Router router;

  RouterFixture()
      : router(make_index(), make_entries(), make_config()) {}

  static rg::Bm25Index make_index() {
    rg::Bm25Index index;
    index.add_document("the cat sat on the mat");  // d0
    index.add_document("the dog ran in the park");  // d1
    index.add_document("a cat and a dog");          // d2
    index.build();
    return index;
  }

  static std::vector<rg::KnowledgeEntry> make_entries() {
    return {
        {"e1", "the cat sat on the mat"},
        {"e2", "the dog ran in the park"},
        {"e3", "a cat and a dog"},
    };
  }

  // 阈值按上述语料的实测得分标定：
  //   "the cat"→1.334（高置信）、"the"→0.659、单字词→0.73~0.96。
  static rg::RouterConfig make_config() {
    rg::RouterConfig c;
    c.direct_threshold = 0.7;
    c.context_threshold = 0.4;
    c.top_k = 2;
    return c;
  }
};

void test_route_l0_control() {
  const RouterFixture f;
  // 中文控制词：命中 L0，绕过检索（chunks 为空），返回固定应答。
  const auto d1 = f.router.route("停止播放");
  CHECK(d1.level == RouteLevel::kL0);
  CHECK(d1.chunks.empty());
  CHECK(!d1.answer.empty());
  CHECK(d1.prompt.empty());
  // 英文控制词（大小写不敏感）。
  const auto d2 = f.router.route("please STOP now");
  CHECK(d2.level == RouteLevel::kL0);
  // 普通词不能触发 L0。
  CHECK(f.router.route("the cat").level != RouteLevel::kL0);
  std::cout << "  [ok] L0 紧急控制：中英文关键词命中、绕过检索" << std::endl;
}

void test_route_l1_direct_answer() {
  const RouterFixture f;
  const auto d = f.router.route("the cat");  // 1.334 ≥ 0.7
  CHECK(d.level == RouteLevel::kL1);
  CHECK(d.chunks.size() == 2);  // 决策携带 Top-K 证据（得分降序）
  CHECK(d.chunks[0].id == "e1");
  CHECK(d.chunks[0].text == "the cat sat on the mat");
  // 直答 = Top-1 知识块文本；不构造 LLM 提示词。
  CHECK(d.answer == "the cat sat on the mat");
  CHECK(d.prompt.empty());
  std::cout << "  [ok] L1 事实直答：Top-1 超阈值直接给出答案、绕过 LLM"
            << std::endl;
}

void test_route_l2_context() {
  const RouterFixture f;
  // "the"（0.659）：≥ 0.4 且 < 0.7 → L2，注入 Top-2 上下文。
  const auto d = f.router.route("the");
  CHECK(d.level == RouteLevel::kL2);
  CHECK(d.chunks.size() == 2);
  CHECK(d.chunks[0].id == "e1");
  CHECK(d.chunks[1].id == "e2");  // 同分按 doc_index 升序
  // 提示词包含参考知识与查询。
  CHECK(d.prompt.find("the cat sat on the mat") != std::string::npos);
  CHECK(d.prompt.find("the dog ran in the park") != std::string::npos);
  CHECK(d.prompt.find(d.query) != std::string::npos);
  CHECK(d.answer.empty());
  std::cout << "  [ok] L2 复杂问题：注入 Top-K 上下文后走 LLM" << std::endl;
}

void test_route_l3_chat() {
  const RouterFixture f;
  // 无命中词 → L3，不注入知识。
  const auto d = f.router.route("hello how are you");
  CHECK(d.level == RouteLevel::kL3);
  CHECK(d.chunks.empty());
  CHECK(d.prompt == "hello how are you");
  CHECK(d.answer.empty());
  std::cout << "  [ok] L3 闲聊/无命中：不注入知识直接走 LLM" << std::endl;
}

void test_route_thresholds_configurable() {
  const RouterFixture f;
  // 调高阈值后同一查询降级：direct=2.0 → "the cat"(1.334) 落入 L2。
  rg::RouterConfig c = RouterFixture::make_config();
  c.direct_threshold = 2.0;
  c.context_threshold = 1.0;
  rg::Bm25Index index = RouterFixture::make_index();
  rg::Router loose(index, RouterFixture::make_entries(), c);
  CHECK(loose.route("the cat").level == RouteLevel::kL2);
  // 阈值再抬高 → L3（无命中可直答/带上下文）。
  c.direct_threshold = 3.0;
  c.context_threshold = 2.0;
  rg::Router strict(index, RouterFixture::make_entries(), c);
  CHECK(strict.route("the cat").level == RouteLevel::kL3);
  // 阈值放宽到 0 → 全部查询落入 L1（只要命中）。
  c.direct_threshold = 0.0;
  c.context_threshold = -1.0;
  rg::Router lenient(index, RouterFixture::make_entries(), c);
  CHECK(lenient.route("the").level == RouteLevel::kL1);
  std::cout << "  [ok] 路由阈值可配：同一查询随阈值升降级" << std::endl;
}

void test_route_empty_knowledge() {
  rg::Bm25Index index;  // 空语料
  index.build();
  rg::Router router(index, {}, RouterFixture::make_config());
  // 空知识库：除 L0 外全部 L3。
  CHECK(router.route("anything").level == RouteLevel::kL3);
  CHECK(router.route("停止").level == RouteLevel::kL0);
  std::cout << "  [ok] 空知识库：无命中一律 L3、L0 规则仍生效" << std::endl;
}

void test_route_chinese_kb() {
  // 中文知识库 + 中文查询（接近真实演示场景）。
  rg::Bm25Index index;
  index.add_document("VoxOrchestra 是端侧全离线语音交互中间件");
  index.add_document("VoxOrchestra 使用多进程架构，网关、管理器与节点各自独立");
  index.add_document("VoxOrchestra 的 RAG 路由分为 L0 到 L3 四层");
  index.build();
  rg::RouterConfig c;
  c.direct_threshold = 0.8;
  c.context_threshold = 0.2;
  c.top_k = 2;
  const rg::Router router(index,
                          {{"k1", "VoxOrchestra 是端侧全离线语音交互中间件"},
                           {"k2", "VoxOrchestra 使用多进程架构，网关、管理器与节点各自独立"},
                           {"k3", "VoxOrchestra 的 RAG 路由分为 L0 到 L3 四层"}},
                          c);
  // "VoxOrchestra 是 什 么"：与 k1 命中 2 个 token，其余文档仅 1 个 → L1。
  const auto d = router.route("VoxOrchestra 是什么");
  CHECK(d.level == RouteLevel::kL1);
  CHECK(d.chunks[0].id == "k1");
  CHECK(d.answer == "VoxOrchestra 是端侧全离线语音交互中间件");
  // "多进程架构 怎么 实现"：与 k2 命中 "多进程架构" 子集 → L2/L1 视得分。
  const auto d2 = router.route("多进程架构怎么实现");
  CHECK(d2.level == RouteLevel::kL2 || d2.level == RouteLevel::kL1);
  CHECK(!d2.chunks.empty());
  std::cout << "  [ok] 中文知识库：查询命中知识条目并正确路由" << std::endl;
}

}  // namespace

int main() {
  std::cout << "rag_test:" << std::endl;
  test_normalize_and_tokenize();
  test_bm25_hand_formula();
  test_bm25_rank_and_determinism();
  test_knowledge_store_load();
  test_knowledge_store_errors();
  test_route_l0_control();
  test_route_l1_direct_answer();
  test_route_l2_context();
  test_route_l3_chat();
  test_route_thresholds_configurable();
  test_route_empty_knowledge();
  test_route_chinese_kb();

  if (g_failures == 0) {
    std::cout << "rag_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "rag_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
