// BM25 文本检索索引（可解释、可手算的轻量 RAG 核心）。
//
// 标准 BM25 打分（token 级，k1 与 b 可配置）：
//   score(q, d) = Σ_{t ∈ q} IDF(t) * f(t,d) * (k1 + 1)
//                       / (f(t,d) + k1 * (1 - b + b * |d| / avgdl))
//   IDF(t) = ln(1 + (N - n_t + 0.5) / (n_t + 0.5))
// 其中 f(t,d) 为词频，|d| 为文档 token 数，avgdl 为语料平均文档长度，
// n_t 为包含 t 的文档数，N 为文档总数。
//
// 用法：add_document × N → build → top_k(query, k)。
// 分词复用 text_normalizer；相同输入永远产出相同分数（无随机源），
// 排名按得分降序、同分按 doc_index 升序（全序，保证确定性）。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace voxorchestra::rag {

class Bm25Index {
 public:
  // k1：词频饱和参数（越大越不惩罚高频）；b：文档长度归一化强度（0-1）。
  struct Options {
    double k1 = 1.5;
    double b = 0.75;
  };

  Bm25Index() = default;
  explicit Bm25Index(Options opts) : opts_(opts) {}

  // 追加一篇文档（build 前调用；文本为空也计入，贡献 0 分）。
  void add_document(const std::string& text);

  // 预计算 IDF、平均文档长度与词汇表（幂等；重复 build 以当前文档集重建）。
  void build();

  std::size_t size() const { return docs_.size(); }  // 文档数

  // 查询与单篇文档（按下标）的 BM25 得分；未 build 或下标越界返回 0。
  double score(const std::string& query, std::size_t doc_index) const;

  // 单 token 的 IDF（对数公式），供手算对照；未 build 或未知 token 返回 0。
  double idf(const std::string& token) const;

  // 一条命中：文档下标 + 得分。
  struct Hit {
    std::size_t doc_index = 0;
    double score = 0.0;
  };

  // 全部命中（得分降序，同分按 doc_index 升序）；查询为空返回空列表。
  std::vector<Hit> rank(const std::string& query) const;

  // Top-K 命中（rank 的前 k 条）；k 为 0 返回空列表。
  std::vector<Hit> top_k(const std::string& query, std::size_t k) const;

 private:
  double score_tokens(const std::vector<std::string>& query_tokens,
                      std::size_t doc_index) const;

  Options opts_;
  std::vector<std::string> doc_texts_;          // 文档原文
  std::vector<std::vector<std::string>> docs_;  // 分词后的文档
  std::vector<std::size_t> doc_sizes_;          // 每篇 token 数
  std::size_t total_tokens_ = 0;

  // build 后有效：
  std::vector<std::string> vocab_;
  std::vector<double> idf_;
  double avg_doc_len_ = 0.0;
  bool built_ = false;
};

}  // namespace voxorchestra::rag
