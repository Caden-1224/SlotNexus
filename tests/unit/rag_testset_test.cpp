// RAG 固定测试集驱动测试：21 条查询逐条断言路由级别与期望一致。
//
// 输入（默认按仓库根相对路径，可用参数覆盖）：
//   --knowledge data/knowledge/knowledge.jsonl
//   --testset   data/knowledge/rag_test_set.jsonl
//   --summary   <路径>（可选）写逐条校准表 JSON 到证据目录
// 阈值使用 RouterConfig 默认值（4.5/2.0）——该值即由本测试集标定，
// 与 config/{mock,taishanpi3m}/session.json 保持一致。
// 测试集为固定资产：期望路径随知识库与阈值冻结，改动需重新标定
// （见 artifacts/rag-baseline/calibration.md）。
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "voxorchestra/rag/knowledge_store.hpp"
#include "voxorchestra/rag/router.hpp"

namespace rg = voxorchestra::rag;

namespace {

int g_failures = 0;

void fail(const std::string& query, const std::string& expect,
          const std::string& actual, double top1) {
  ++g_failures;
  std::cerr << "FAIL 期望 " << expect << " 实际 " << actual << " (top1="
            << top1 << ") query=" << query << std::endl;
}

// 从 argv[0] 推导仓库根（build-wsl/tests/unit/ → 仓库根，三层上溯）。
std::string repo_root(const char* argv0) {
  std::filesystem::path p =
      std::filesystem::absolute(std::filesystem::path(argv0).parent_path());
  for (int i = 0; i < 3 && !p.empty(); ++i) {
    p = p.parent_path();
  }
  return p.string();
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

std::vector<TestCase> load_testset(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("无法读取测试集: " + path);
  }
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

}  // namespace

int main(int argc, char** argv) {
  const std::string root = repo_root(argv[0]);
  const std::string knowledge_path =
      opt(argv[0], argc, argv, "--knowledge",
          root + "/data/knowledge/knowledge.jsonl");
  const std::string testset_path =
      opt(argv[0], argc, argv, "--testset",
          root + "/data/knowledge/rag_test_set.jsonl");
  const std::string summary_path =
      opt(argv[0], argc, argv, "--summary", "");

  const rg::KnowledgeStore store(knowledge_path);
  rg::Bm25Index index;
  for (const auto& e : store.entries()) {
    index.add_document(e.text);
  }
  index.build();
  const rg::Router router(std::move(index), store.entries(), rg::RouterConfig{});

  const auto cases = load_testset(testset_path);
  std::cout << "rag_testset_test: 知识库 " << store.size() << " 条，测试集 "
            << cases.size() << " 条（direct=" << router.config().direct_threshold
            << " context=" << router.config().context_threshold << "）"
            << std::endl;

  nlohmann::json summary;
  summary["knowledge"] = knowledge_path;
  summary["testset"] = testset_path;
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
