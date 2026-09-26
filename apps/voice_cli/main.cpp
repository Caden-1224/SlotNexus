// voice_cli：TCP Gateway 外部客户端（联调入口）。
//
// 用法：voice_cli [选项]
//  --gateway <host:port>   网关地址（默认 127.0.0.1:9100）
//  --text <查询>           文本模式（直接发送查询文本）
//  --wav <路径>            WAV 模式（绝对路径直接用；相对名按 session
//                          fixture 目录解析，如 voice.wav）
//  --cancel-ms <N>         推理开始 N 毫秒后发送 cancel（打断验证；0 不发）
//  --timeout-ms <N>        每条请求的等待超时（默认 60000；硬件后端推理
//                          可达数十秒，按需调大）
//  --json                  打印原始响应 JSON（默认打印人类可读摘要）
//
// 流程：setup（Manager 分配 work_id）→ inference（text/wav）→ exit。
// cancel 用独立连接发送（网关按 work_id 路由，不依赖连接归属），
// 用于验证 Thinking/Speaking 阶段打断与晚到数据丢弃。
// SIGINT 时若在途推理未结束，先发 cancel 再退出（退出码 0）。
//
// 退出码：响应 status=ok/cancelled 返回 0；error/超时/连接失败返回 1。
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "slotnexus/protocol/message_envelope.hpp"

namespace {

using slotnexus::protocol::MessageEnvelope;
using slotnexus::protocol::MessageType;
using slotnexus::protocol::ProtocolError;

// 最小 TCP 客户端：连接、整帧发送、按行接收（带超时）。
// 与测试客户端/脚本一致：NDJSON 一行一帧，超时由调用方控制。
class TcpConn {
public:
 bool connect(const std::string& host, std::uint16_t port,
              std::chrono::milliseconds timeout) {
   const struct addrinfo hints = {.ai_family = AF_UNSPEC,
                                  .ai_socktype = SOCK_STREAM};
   struct addrinfo* res = nullptr;
   const std::string service = std::to_string(port);
   if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0 ||
       res == nullptr) {
     return false;
   }
   for (auto* ai = res; ai != nullptr; ai = ai->ai_next) {
     fd_ = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
     if (fd_ < 0) {
       continue;
     }
     // 非阻塞连接 + 超时轮询（避免 connect 无限挂起）。
     ::fcntl(fd_, F_SETFL, O_NONBLOCK);
     if (::connect(fd_, ai->ai_addr, ai->ai_addrlen) == 0 ||
         errno == EINPROGRESS) {
       const auto deadline = std::chrono::steady_clock::now() + timeout;
       for (;;) {
         const auto now = std::chrono::steady_clock::now();
         if (now >= deadline) {
           break;
         }
         fd_set wfds;
         FD_ZERO(&wfds);
         FD_SET(fd_, &wfds);
         const auto left = std::chrono::duration_cast<std::chrono::microseconds>(
                               deadline - now).count();
         struct timeval tv = {.tv_sec = static_cast<long>(left / 1000000),
                              .tv_usec = static_cast<long>(left % 1000000)};
         const int r = ::select(fd_ + 1, nullptr, &wfds, nullptr, &tv);
         if (r > 0 && FD_ISSET(fd_, &wfds)) {
           int err = 0;
           socklen_t len = sizeof(err);
           ::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len);
           if (err != 0) {
             break;  // 连接被拒/不可达，立即失败
           }
           // SO_ERROR==0 仍可能是 RST 未到协议栈的竞态窗口；
           // getpeername 只对已建立连接成功，作为权威确认。
           struct sockaddr_storage peer;
           socklen_t plen = sizeof(peer);
           if (::getpeername(fd_,
                             reinterpret_cast<struct sockaddr*>(&peer),
                             &plen) == 0) {
             ::fcntl(fd_, F_SETFL, 0);  // 恢复阻塞
             ::freeaddrinfo(res);
             return true;
           }
           break;  // SO_ERROR=0 但连接未建立：视为失败
         } else if (r < 0 && errno != EINTR) {
           break;
         }
       }
     }
     ::close(fd_);
     fd_ = -1;
   }
   ::freeaddrinfo(res);
   return false;
 }

 bool send_line(const std::string& line) {
   std::string data = line + "\n";
   std::size_t sent = 0;
   while (sent < data.size()) {
     const ssize_t n =
         ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
     if (n <= 0) {
       return false;
     }
     sent += static_cast<std::size_t>(n);
   }
   return true;
 }

 // 读一行（以 \n 结尾）；超时返回 false；对端关闭返回 false。
 bool recv_line(std::string& line, std::chrono::milliseconds timeout) {
   line.clear();
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (line.find('\n') == std::string::npos) {
     const auto now = std::chrono::steady_clock::now();
     if (now >= deadline) {
       return false;
     }
     fd_set rfds;
     FD_ZERO(&rfds);
     FD_SET(fd_, &rfds);
     const auto left = std::chrono::duration_cast<std::chrono::microseconds>(
                           deadline - now).count();
     struct timeval tv = {.tv_sec = static_cast<long>(left / 1000000),
                          .tv_usec = static_cast<long>(left % 1000000)};
     const int r = ::select(fd_ + 1, &rfds, nullptr, nullptr, &tv);
     if (r <= 0) {
       if (r < 0 && errno == EINTR) {
         continue;
       }
       return false;  // 超时
     }
     char buf[4096];
     const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
     if (n <= 0) {
       return false;  // 对端关闭
     }
     line.append(buf, static_cast<std::size_t>(n));
     if (line.size() > 8 * 1024 * 1024) {
       return false;  // 超长帧保护
     }
   }
   if (!line.empty() && line.back() == '\n') {
     line.pop_back();
   }
   return true;
 }

 void close() {
   if (fd_ >= 0) {
     ::close(fd_);
     fd_ = -1;
   }
 }

 ~TcpConn() { close(); }

private:
 int fd_ = -1;
};

// 单条请求：构造信封 → 发送 → 读响应（带超时）。
// 返回 false 表示连接/超时失败（非协议错误，协议错误在响应 error 信封中）。
// silent=true 时失败不打印（尽力而为的请求如 cancel 使用）。
bool exchange(TcpConn& c, const MessageEnvelope& req,
             std::chrono::milliseconds timeout, MessageEnvelope& reply,
             bool silent = false) {
 if (!c.send_line(req.to_json())) {
   if (!silent) {
     std::cerr << "发送失败（连接已断开？）" << std::endl;
   }
   return false;
 }
 std::string line;
 if (!c.recv_line(line, timeout)) {
   if (!silent) {
     std::cerr << "等待响应超时（" << timeout.count() << " ms）或连接关闭"
               << std::endl;
   }
   return false;
 }
 try {
   reply = MessageEnvelope::from_json(line);
   return true;
 } catch (const ProtocolError& e) {
   if (!silent) {
     std::cerr << "响应解析失败: " << e.what() << std::endl;
   }
   return false;
 }
}

// 发送 cancel（独立连接，网关按 work_id 路由）。
// cancel 为尽力而为请求：网关同步转发下晚到取消无确认是预期行为
// （取消排在在途推理之后，见 demo_mock_session.sh 的已知限制说明），
// 因此静默失败，不打扰主流程输出。
bool send_cancel(const std::string& host, std::uint16_t port,
                const std::string& work_id, const std::string& request_id,
                std::chrono::milliseconds timeout) {
 TcpConn c;
 if (!c.connect(host, port, timeout)) {
   return false;
 }
 MessageEnvelope req;
 req.set_type(MessageType::kCancel);
 req.set_work_id(work_id);
 req.set_request_id(request_id);
 MessageEnvelope reply;
 return exchange(c, req, timeout, reply, /*silent=*/true);
}

// 人类可读摘要（非 --json 模式）。
void print_summary(const std::string& tag, const MessageEnvelope& reply) {
 std::cout << tag
           << " type="
           << slotnexus::protocol::message_type_to_string(reply.type());
 if (!reply.work_id().empty()) {
   std::cout << " work_id=" << reply.work_id();
 }
 if (!reply.request_id().empty()) {
   std::cout << " request_id=" << reply.request_id();
 }
 if (reply.type() == MessageType::kError) {
   std::cout << " error_code=" << reply.error().code
             << " message=" << reply.error().message;
   std::cout << std::endl;
   return;
 }
 if (reply.payload().empty()) {
   std::cout << std::endl;
   return;
 }
 const auto& p = reply.payload();
 std::cout << " status=" << p.value("status", std::string())
           << " route=" << p.value("route", std::string())
           << " tokens=" << p.value("token_count", 0)
           << " pcm_frames=" << p.value("pcm_frames", 0)
           << " dropped_sent=" << p.value("dropped_sentences", 0)
           << " dropped_pcm=" << p.value("dropped_pcm_frames", 0);
 if (p.contains("final_text")) {
   std::cout << " final_text=" << p["final_text"].get<std::string>();
 }
 if (p.contains("wav_path")) {
   std::cout << " wav=" << p["wav_path"].get<std::string>();
 }
 if (p.contains("state")) {
   std::cout << " state=" << p["state"].get<std::string>();
 }
 std::cout << std::endl;
}

std::atomic<bool> g_interrupted{false};
volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int /*sig*/) { g_stop = 1; }

}  // namespace

int main(int argc, char** argv) {
 std::string gateway = "127.0.0.1:9100";
 std::string text_mode;    // --text 时非空
 std::string wav_mode;     // --wav 时非空
 int cancel_ms = 0;
 int timeout_ms = 60000;
 bool json_output = false;

 for (int i = 1; i < argc; ++i) {
   const std::string arg = argv[i];
   if (arg == "--json") {
     json_output = true;
   } else if (i + 1 < argc) {
     const std::string val = argv[i + 1];
     if (arg == "--gateway") {
       gateway = val;
       ++i;
     } else if (arg == "--text") {
       text_mode = val;
       ++i;
     } else if (arg == "--wav") {
       wav_mode = val;
       ++i;
     } else if (arg == "--cancel-ms") {
       try {
         cancel_ms = std::stoi(val);
       } catch (...) {
       }
       ++i;
     } else if (arg == "--timeout-ms") {
       try {
         timeout_ms = std::stoi(val);
       } catch (...) {
       }
       ++i;
     }
   }
 }
 if (text_mode.empty() && wav_mode.empty()) {
   std::cerr << "用法: voice_cli --text <查询> | --wav <路径> [--cancel-ms N]"
             << " [--gateway host:port] [--json]" << std::endl;
   return 1;
 }
 if (!text_mode.empty() && !wav_mode.empty()) {
   std::cerr << "--text 与 --wav 互斥" << std::endl;
   return 1;
 }

 const auto colon = gateway.find_last_of(':');
 if (colon == std::string::npos) {
   std::cerr << "网关地址须为 host:port: " << gateway << std::endl;
   return 1;
 }
 const std::string host = gateway.substr(0, colon);
 const std::uint16_t port = static_cast<std::uint16_t>(
     std::stoul(gateway.substr(colon + 1)));
 const auto timeout = std::chrono::milliseconds(timeout_ms);

 std::signal(SIGINT, handle_signal);
 std::signal(SIGTERM, handle_signal);

 TcpConn conn;
 if (!conn.connect(host, port, timeout)) {
   std::cerr << "无法连接网关 " << gateway << std::endl;
   return 1;
 }

 // ---------- 1. setup：Manager 分配 work_id ----------
 MessageEnvelope setup;
 setup.set_type(MessageType::kSetup);
 setup.set_request_id("cli-setup-1");
 MessageEnvelope reply;
 if (!exchange(conn, setup, timeout, reply)) {
   return 1;  // exchange 内部已打印具体错误
 }
 if (reply.type() != MessageType::kAck) {
   if (!json_output) {
     print_summary("setup", reply);
   }
   return 1;
 }
 const std::string work_id = reply.work_id();
 if (json_output) {
   std::cout << reply.to_json() << std::endl;
 } else {
   std::cout << "setup -> work_id=" << work_id << std::endl;
 }

 // ---------- 2. inference：text/wav，可附带延迟 cancel ----------
 MessageEnvelope infer;
 infer.set_type(MessageType::kInference);
 infer.set_work_id(work_id);
 infer.set_request_id("cli-infer-1");
 nlohmann::json payload;
 if (!text_mode.empty()) {
   payload = {{"mode", "text"}, {"text", text_mode}};
 } else {
   payload = {{"mode", "wav"}, {"wav", wav_mode}};
 }
 infer.set_payload(std::move(payload));

 std::thread canceller;
 if (cancel_ms > 0) {
   // 独立连接发送 cancel：与主连接解耦，避免并发写同一 socket。
   canceller = std::thread([host, port, work_id, cancel_ms] {
     std::this_thread::sleep_for(std::chrono::milliseconds(cancel_ms));
     if (g_stop != 0) {
       return;  // 已因用户中断退出，不再发 cancel
     }
     send_cancel(host, port, work_id, "cli-cancel-1",
                 std::chrono::milliseconds(3000));
   });
 }

 const auto t0 = std::chrono::steady_clock::now();
 const bool ok = exchange(conn, infer, timeout, reply);
 const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - t0).count();
 if (canceller.joinable()) {
   canceller.join();
 }

 if (!ok) {
   return 1;
 }
 if (json_output) {
   std::cout << reply.to_json() << std::endl;
 } else {
   print_summary("inference", reply);
   std::cout << "耗时 " << ms << " ms" << std::endl;
 }

 // 用户中断且推理尚未返回：补发 cancel 后以成功退出。
 if (g_stop != 0) {
   send_cancel(host, port, work_id, "cli-cancel-2", timeout);
 }

 // ---------- 3. exit：释放任务 ----------
 MessageEnvelope exit_req;
 exit_req.set_type(MessageType::kExit);
 exit_req.set_work_id(work_id);
 exit_req.set_request_id("cli-exit-1");
 MessageEnvelope exit_reply;
 const bool exit_ok = exchange(conn, exit_req, timeout, exit_reply);
 if (!exit_ok) {
   return 1;  // exchange 内部已打印具体错误
 }
 if (json_output) {
   std::cout << exit_reply.to_json() << std::endl;
 } else {
   print_summary("exit", exit_reply);
 }
 if (exit_reply.type() == MessageType::kError) {
   return 1;
 }
 // 取消是预期路径（联调用），不算失败。
 return 0;
}
