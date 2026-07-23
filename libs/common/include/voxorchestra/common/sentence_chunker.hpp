// 流式分句器：把 token 流切分为完整句子（TTS 前一级）。
//
// 规则（确定性、可测试）：
//   - 句末标点 。！？!?;； 与换行符 \n 触发切分；中文标点为 UTF-8 三字节
//     序列，按字节模式识别；切分标点归属于前一句，换行符不进入句子；
//   - 连续句末标点只保留首个；句子不以标点开头（句首标点丢弃）；
//   - 逗号、顿号等非句末标点不切分；
//   - feed(text) 返回本次输入中完成的句子（可能为空）；
//   - flush() 返回未完成的收尾句子（仅含尾部标点时丢弃），并重置状态。
//
// 用法（流式）：
//   chunker.feed("第一句。第二句未完成") → ["第一句。"]
//   chunker.feed("，补全。")             → ["第二句未完成，补全。"]
//   chunker.flush()                      → []（已全部完成）
#pragma once

#include <string>
#include <vector>

namespace voxorchestra::common {

class SentenceChunker {
 public:
  // 处理一段文本，返回其中完整的句子（标点归属前一句）。
  std::vector<std::string> feed(const std::string& text);

  // 返回未完成的收尾句子，并重置内部状态。
  std::vector<std::string> flush();

 private:
  std::string cur_;            // 当前句缓冲
  bool split_pending_ = false; // 上一句已切分、等待下一句内容
};

}  // namespace voxorchestra::common
