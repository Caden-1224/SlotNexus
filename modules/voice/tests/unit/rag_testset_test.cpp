// RAG 固定测试集驱动测试：21 条查询逐条断言路由级别与期望一致。
// Author: Caden
//
// 输入（默认按仓库根相对路径，可用参数覆盖）：
//   --knowledge data/knowledge/knowledge.jsonl
//   --testset   data/knowledge/rag_test_set.jsonl
//   --summary   <路径>（可选）写逐条校准表 JSON 到证据目录
// 阈值使用 RouterConfig 默认值（4.5/2.0）——该值即由本测试集标定，
// 与 config/{mock,taishanpi3m}/session.json 保持一致。
// 测试集为固定资产：期望路径随知识库与阈值冻结，改动需重新运行本测试标定。
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "test_paths.hpp"
#include "slotnexus/rag/knowledge_store.hpp"
#include "slotnexus/rag/router.hpp"

namespace rg = slotnexus::rag;

namespace {

int g_failures = 0;

void fail(const std::string& query, const std::string& expect,
          const std::string& actual, double top1) {
  ++g_failures;
  std::cerr << "FAIL 期望 " << expect << " 实际 " << actual << " (top1="
            << top1 << ") query=" << query << std::endl;
}

std::string opt(const char* argv0, int argc, char** argv,
                const std::string& name, const std::string& def) {
  for (int i = 1; i < argc - 1; ++i) {
    if (argv[i] == name) {
      return argv[i + 1];
    }
  }
  return def;
}

struct TestCase {
  std::string query;
  std::string expect;
  std::string note;
};

// 默认测试集内嵌在二进制中：真实/历史 21 条固定集已外置归档，默认测试
// 不再依赖宿主机 data/knowledge/rag_test_set.jsonl。
constexpr const char* kEmbeddedTestset = R"json(
{"query": "停止播放", "expect": "l0", "note": "控制词命中 L0，绕过检索"}
{"query": "暂停一下", "expect": "l0", "note": "控制词命中 L0"}
{"query": "闭嘴", "expect": "l0", "note": "控制词命中 L0"}
{"query": "stop the music", "expect": "l0", "note": "英文控制词命中 L0"}
{"query": "取消后旧数据怎么处理", "expect": "l0", "note": "已知边界：L0 关键词为子串匹配，含\"取消\"即触发；固化现状并记录，不属本次修正范围"}
{"query": "多进程架构怎么实现", "expect": "l1", "note": "top1=8.44 命中 k-arch，事实直答"}
{"query": "RAG 路由分为哪几层", "expect": "l1", "note": "top1=9.94 命中 k-rag"}
{"query": "音频格式是什么", "expect": "l1", "note": "top1=7.65 命中 k-audio"}
{"query": "队列满了怎么办", "expect": "l1", "note": "top1=6.75 命中 k-queue"}
{"query": "端侧全离线是什么意思", "expect": "l1", "note": "top1=8.64 命中 k-slotnexus"}
{"query": "Backend 为什么可替换", "expect": "l1", "note": "top1=6.96 命中 k-backend"}
{"query": "Node Runtime 做什么", "expect": "l2", "note": "top1=3.77 部分命中，带上下文走 LLM"}
{"query": "Node Runtime 是什么", "expect": "l2", "note": "top1=3.77 部分命中（同分查询确定性）"}
{"query": "SlotNexus 的架构是怎样的", "expect": "l2", "note": "top1=3.43 部分命中"}
{"query": "generation 有什么用", "expect": "l2", "note": "top1=3.03 部分命中"}
{"query": "Backend 是什么", "expect": "l2", "note": "top1=2.98 部分命中"}
{"query": "今天天气怎么样", "expect": "l3", "note": "无命中，纯 LLM"}
{"query": "讲个笑话", "expect": "l3", "note": "噪声命中 k-arch 1.687，context=2.0 挡回，不注入伪知识"}
{"query": "帮我写一首诗", "expect": "l3", "note": "噪声命中 k-audio 1.086"}
{"query": "你好", "expect": "l3", "note": "无命中"}
{"query": "what time is it", "expect": "l3", "note": "无命中"}
)json";

std::vector<TestCase> parse_testset(std::istream& in) {
  std::vector<TestCase> cases;
  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    if (line.empty() || line[0] == '#') {
      continue;
    }
    nlohmann::json obj;
    try {
      obj = nlohmann::json::parse(line);
    } catch (const nlohmann::json::exception& e) {
      throw std::runtime_error("测试集第 " + std::to_string(lineno) +
                               " 行 JSON 解析失败: " + e.what());
    }
    TestCase tc;
    tc.query = obj.value("query", std::string());
    tc.expect = obj.value("expect", std::string());
    tc.note = obj.value("note", std::string());
    if (tc.query.empty() || tc.expect.empty()) {
      throw std::runtime_error("测试集第 " + std::to_string(lineno) +
                               " 行缺少 query/expect");
    }
    cases.push_back(std::move(tc));
  }
  return cases;
}

std::vector<TestCase> load_testset(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("无法读取测试集: " + path);
  }
  return parse_testset(in);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string root = vox_test::Root(argv[0]);
  const std::string knowledge_path =
      opt(argv[0], argc, argv, "--knowledge",
          root + "/data/knowledge/knowledge.jsonl");
  const std::string testset_path =
      opt(argv[0], argc, argv, "--testset", "");
  const std::string summary_path =
      opt(argv[0], argc, argv, "--summary", "");

  const rg::KnowledgeStore store(knowledge_path);
  rg::Bm25Index index;
  for (const auto& e : store.entries()) {
    index.add_document(e.text);
  }
  index.build();
  const rg::Router router(std::move(index), store.entries(), rg::RouterConfig{});

  std::vector<TestCase> cases;
  if (testset_path.empty()) {
    std::istringstream embedded(kEmbeddedTestset);
    cases = parse_testset(embedded);
  } else {
    cases = load_testset(testset_path);
  }
  std::cout << "rag_testset_test: 知识库 " << store.size() << " 条，测试集 "
            << cases.size() << " 条（direct=" << router.config().direct_threshold
            << " context=" << router.config().context_threshold << "）"
            << std::endl;

  nlohmann::json summary;
  summary["knowledge"] = knowledge_path;
  summary["testset"] = testset_path.empty() ? "<embedded>" : testset_path;
  summary["direct_threshold"] = router.config().direct_threshold;
  summary["context_threshold"] = router.config().context_threshold;
  summary["cases"] = nlohmann::json::array();
  std::size_t passed = 0;

  for (const auto& tc : cases) {
    const auto d = router.route(tc.query);
    const std::string actual = rg::to_string(d.level);
    nlohmann::json j;
    j["query"] = tc.query;
    j["expect"] = tc.expect;
    j["actual"] = actual;
    j["top1_score"] = d.top1_score;
    j["note"] = tc.note;
    summary["cases"].push_back(std::move(j));
    const bool ok = (actual == tc.expect);
    std::cout << (ok ? "  [ok] " : "  [!!] ") << tc.query << " → " << actual
              << " (期望 " << tc.expect << ", top1=" << d.top1_score << ")"
              << std::endl;
    if (ok) {
      ++passed;
    } else {
      fail(tc.query, tc.expect, actual, d.top1_score);
    }
  }

  summary["total"] = cases.size();
  summary["passed"] = passed;
  summary["failures"] = g_failures;

  if (!summary_path.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(summary_path).parent_path(), ec);
    std::ofstream out(summary_path);
    if (out) {
      out << summary.dump(2) << "\n";
      std::cout << "校准表已写入 " << summary_path << std::endl;
    } else {
      std::cerr << "警告: 无法写入 summary " << summary_path << std::endl;
    }
  }

  if (g_failures == 0) {
    std::cout << "rag_testset_test 全部通过（" << passed << "/" << cases.size()
              << "）" << std::endl;
    return 0;
  }
  std::cerr << "rag_testset_test 失败 " << g_failures << " 条（通过 "
            << passed << "/" << cases.size() << "）" << std::endl;
  return 1;
}
