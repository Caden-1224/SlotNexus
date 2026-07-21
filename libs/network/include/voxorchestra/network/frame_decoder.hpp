// NDJSON 增量解帧器。
//
// TCP 是字节流，不保留一次 send() 的边界：一次 read() 可能返回半帧、
// 数帧或 0 字节。本类维护内部缓冲，把任意字节块输入切成完整帧：
//
//   - 帧以 '\n' 结尾；兼容 '\r\n'（行尾 '\r' 被剥离）；
//   - 空行（空白行）不产生输出帧；
//   - 单帧内容长度超过 max_frame_bytes 视为协议错误（kOversized），
//     此时内部缓冲被清空，调用方应关闭连接。
//
// 本类只负责"按行切分"，JSON 合法性由上层（MessageEnvelope）校验。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace voxorchestra::network {

enum class FrameResult {
  kOk,         // 正常处理完本次输入
  kOversized,  // 出现超过上限的帧（无论是否已换行），缓冲已清空
};

class NdjsonFrameDecoder {
 public:
  // max_frame_bytes：单帧内容的最大字节数（不含换行符）。
  explicit NdjsonFrameDecoder(std::size_t max_frame_bytes = 1 << 20);

  // 输入任意长度的字节块，把完整帧追加到 frames。
  // 返回 kOversized 后解码器保持可用，但调用方应关闭连接。
  FrameResult feed(std::string_view chunk, std::vector<std::string>& frames);

  // 尚未成帧的剩余字节数（尾部半帧）。
  std::size_t partial_size() const;

  // 清空缓冲与状态。
  void reset();

 private:
  std::size_t max_frame_bytes_;
  std::string buffer_;
};

}  // namespace voxorchestra::network
