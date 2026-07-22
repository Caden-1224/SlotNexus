#include "voxorchestra/rag/bm25.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

#include "voxorchestra/rag/text_normalizer.hpp"

namespace voxorchestra::rag {

void Bm25Index::add_document(const std::string& text) {
  doc_texts_.push_back(text);
  auto tokens = tokenize(text);
  doc_sizes_.push_back(tokens.size());
  total_tokens_ += tokens.size();
  docs_.push_back(std::move(tokens));
}

void Bm25Index::build() {
  // 文档级词频：token → 文档下标 → 出现次数。
  std::map<std::string, std::map<std::size_t, std::size_t>> df;
  for (std::size_t d = 0; d < docs_.size(); ++d) {
    std::map<std::string, std::size_t> local;  // 文档内词频
    for (const auto& t : docs_[d]) {
      ++local[t];
    }
    for (const auto& [t, f] : local) {
      df[t][d] = f;
    }
  }

  vocab_.clear();
  idf_.clear();
  const double n = static_cast<double>(docs_.size());
  for (const auto& [t, doc_map] : df) {
    const double n_t = static_cast<double>(doc_map.size());
    // ln(1 + (N - n_t + 0.5) / (n_t + 0.5))：全部文档都含该词时仍为正数。
    idf_.push_back(std::log(1.0 + (n - n_t + 0.5) / (n_t + 0.5)));
    vocab_.push_back(t);
  }
  avg_doc_len_ = (docs_.empty()) ? 0.0
                                 : static_cast<double>(total_tokens_) /
                                       static_cast<double>(docs_.size());
  built_ = true;
}

double Bm25Index::score(const std::string& query, std::size_t doc_index) const {
  if (!built_ || doc_index >= docs_.size()) {
    return 0.0;
  }
  return score_tokens(tokenize(query), doc_index);
}

double Bm25Index::idf(const std::string& token) const {
  if (!built_) {
    return 0.0;
  }
  const auto it = std::lower_bound(vocab_.begin(), vocab_.end(), token);
  if (it == vocab_.end() || *it != token) {
    return 0.0;
  }
  return idf_[static_cast<std::size_t>(it - vocab_.begin())];
}

std::vector<Bm25Index::Hit> Bm25Index::rank(const std::string& query) const {
  std::vector<Hit> hits;
  if (!built_) {
    return hits;
  }
  const auto query_tokens = tokenize(query);
  if (query_tokens.empty()) {
    return hits;
  }
  hits.reserve(docs_.size());
  for (std::size_t d = 0; d < docs_.size(); ++d) {
    const double s = score_tokens(query_tokens, d);
    if (s > 0.0) {
      hits.push_back({d, s});
    }
  }
  std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
    if (a.score != b.score) {
      return a.score > b.score;  // 得分降序
    }
    return a.doc_index < b.doc_index;  // 同分按 doc_index 升序（全序）
  });
  return hits;
}

std::vector<Bm25Index::Hit> Bm25Index::top_k(const std::string& query,
                                             std::size_t k) const {
  auto hits = rank(query);
  if (hits.size() > k) {
    hits.resize(k);
  }
  return hits;
}

double Bm25Index::score_tokens(const std::vector<std::string>& query_tokens,
                               std::size_t doc_index) const {
  // 查询 token 去重（同一 token 只贡献一次 IDF × 词频项）。
  std::vector<std::string> unique;
  for (const auto& t : query_tokens) {
    if (std::find(unique.begin(), unique.end(), t) == unique.end()) {
      unique.push_back(t);
    }
  }

  double total = 0.0;
  const double doc_len = static_cast<double>(doc_sizes_[doc_index]);
  const double denom_scale =
      (avg_doc_len_ > 0.0) ? doc_len / avg_doc_len_ : 1.0;
  const double k1 = opts_.k1;
  const double norm = k1 * (1.0 - opts_.b + opts_.b * denom_scale);

  for (const auto& t : unique) {
    const auto v = std::lower_bound(vocab_.begin(), vocab_.end(), t);
    if (v == vocab_.end() || *v != t) {
      continue;  // 查询词不在语料中：IDF 为 0，跳过
    }
    const std::size_t vid = static_cast<std::size_t>(v - vocab_.begin());
    const std::size_t f =
        std::count(docs_[doc_index].begin(), docs_[doc_index].end(), t);
    if (f == 0) {
      continue;
    }
    total += idf_[vid] * (static_cast<double>(f) * (k1 + 1.0)) /
             (static_cast<double>(f) + norm);
  }
  return total;
}

}  // namespace voxorchestra::rag
