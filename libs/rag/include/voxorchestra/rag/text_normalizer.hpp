// 文本规范化与分词（RAG 共用，确定性、可手算）。
//
// 规则：
//   - 全角字符（U+FF01..U+FF5E）折叠为半角，全角空格 U+3000 折叠为普通空格；
//   - 英文字母统一小写；
//   - 分词：CJK 字符逐字成 token；ASCII 字母/数字连续串整体成一个 token；
//   - 其余字符（标点、符号）既不是 token，也不参与文档长度统计。
// 相同输入永远产出相同输出，供 BM25 打分与测试断言精确内容。
#pragma once

#include <string>
#include <vector>

namespace voxorchestra::rag {

// 规范化文本：小写、全角折半角、空白折叠为单个空格、去掉非字母数字/CJK 符号。
std::string normalize_text(const std::string& text);

// 分词：CJK 逐字 + 字母数字串；空串返回空列表。
std::vector<std::string> tokenize(const std::string& text);

// 判读辅助（测试与实现共用）：是否 CJK 基本区字符（U+4E00..U+9FFF）。
inline bool is_cjk(char32_t c) { return c >= 0x4E00 && c <= 0x9FFF; }

}  // namespace voxorchestra::rag
