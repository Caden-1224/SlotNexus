// 传输层统一错误类型。
#pragma once

#include <stdexcept>
#include <string>

namespace voxorchestra::transport {

enum class TransportErrorCode {
  kTimeout,     // 等待超时（deadline 到期）
  kClosed,      // 通道已关闭或 context 已终止
  kSendFailed,  // 发送失败
  kRecvFailed,  // 接收失败
  kInterrupted, // 被中断（EINTR）
};

class TransportError : public std::runtime_error {
 public:
  TransportError(TransportErrorCode code, const std::string& message)
      : std::runtime_error(message), code_(code) {}
  TransportErrorCode code() const { return code_; }

 private:
  TransportErrorCode code_;
};

}  // namespace voxorchestra::transport
