// EdgeGateway：外部客户端（voice_cli / 测试）的 TCP 接入点。
//
// 职责：
//  1. 接收 NDJSON 请求，解析为 MessageEnvelope（复用 protocol 校验）；
//  2. 合法请求回 ack 信封（回显 request_id 等关联字段）；
//  3. 非法请求（坏 JSON/未知版本/未知类型/缺字段）回结构化错误信封，
//     连接保持可用；
//  4. 超长帧由连接层关闭（不可信客户端不回复）。
//
// 起，合法请求将转发给 Unit Manager 执行 action，ack 只是当前占位。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "voxorchestra/network/event_loop.hpp"
#include "voxorchestra/network/tcp_server.hpp"

namespace voxorchestra::gateway {

class EdgeGateway {
public:
 EdgeGateway(network::EventLoop* loop, const std::string& host, std::uint16_t port);
 ~EdgeGateway() = default;

 EdgeGateway(const EdgeGateway&) = delete;
 EdgeGateway& operator=(const EdgeGateway&) = delete;

 // 在 loop 线程开始监听。
 void start();
 // 线程安全：停止监听并断开所有连接。
 void stop();

 std::uint16_t local_port() const { return server_.local_port(); }

private:
 void handle_message(const std::shared_ptr<network::TcpConnection>& conn,
                     const std::string& frame);
 void handle_protocol_error(const std::shared_ptr<network::TcpConnection>& conn,
                            const std::string& message);

 network::EventLoop* loop_;
 network::TcpServer server_;
};

}  // namespace voxorchestra::gateway
