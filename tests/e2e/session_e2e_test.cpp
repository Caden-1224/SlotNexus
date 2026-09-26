// Session 进程级端到端测试（验收）：
//
// 真实进程拓扑：
//  client --TCP--> edge_gateway --ZMQ--> unit_manager --ZMQ--> session_node(19210)
// session_node 内含：固定 WAV → Fake ASR → 真实 BM25 L0-L3 路由 →
// （Fake LLM）→ 分句 → Fake TTS → WAV 输出；有界队列 + generation 取消。
//
// 验收标准：
//  1. 四类路由走对路径（L0 控制 / L1 直答 / L2 带上下文 / L3 闲聊）；
//  2. 固定 WAV 完整链路输出可验证的 WAV 文件；
//  3. 取消传播：inference 在途时 cancel 生效，旧 token/PCM 不进入新请求
//     （gateway/manager 为同步转发，取消场景直连 session_node 验证）；
//  4. taskinfo/exit 生命周期正确；
//  5. SIGTERM 三进程全部优雅退出（退出码 0）。
#include "slotnexus/protocol/message_envelope.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zmq.hpp>

using namespace std::chrono_literals;

namespace ep = slotnexus::protocol;
using ep::MessageEnvelope;
using ep::MessageType;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
 do {                                                                       \
   if (!(cond)) {                                                           \
     ++g_failures;                                                          \
     std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " << #cond   \
               << std::endl;                                                \
   }                                                                        \
 } while (0)

bool wait_until(const std::function<bool()>& cond, std::chrono::milliseconds timeout) {
 const auto deadline = std::chrono::steady_clock::now() + timeout;
 while (std::chrono::steady_clock::now() < deadline) {
   if (cond()) {
     return true;
   }
   std::this_thread::sleep_for(2ms);
 }
 return cond();
}

// TCP 客户端（与 fake_nodes_e2e_test 相同，一次 recv 多行不丢失）。
class TestClient {
public:
 int fd = -1;
 std::string rx_buffer_;

 bool connect_to(std::uint16_t port) {
   fd = ::socket(AF_INET, SOCK_STREAM, 0);
   if (fd < 0) {
     return false;
   }
   struct sockaddr_in addr {};
   addr.sin_family = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   addr.sin_port = htons(port);
   return ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0;
 }

 bool send_all(const std::string& data) {
   std::size_t sent = 0;
   while (sent < data.size()) {
     const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
     if (n <= 0) {
       return false;
     }
     sent += static_cast<std::size_t>(n);
   }
   return true;
 }

 bool recv_line(std::string& line, std::chrono::milliseconds timeout) {
   line.clear();
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (std::chrono::steady_clock::now() < deadline) {
     const std::size_t nl = rx_buffer_.find('\n');
     if (nl != std::string::npos) {
       line = rx_buffer_.substr(0, nl);
       rx_buffer_.erase(0, nl + 1);
       return true;
     }
     struct pollfd pfd {.fd = fd, .events = POLLIN, .revents = 0};
     const int r = ::poll(&pfd, 1, 200);
     if (r <= 0) {
       continue;
     }
     char buf[4096];
     const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
     if (n <= 0) {
       return false;
     }
     rx_buffer_.append(buf, static_cast<std::size_t>(n));
   }
   return false;
 }

 ~TestClient() {
   if (fd >= 0) {
     ::close(fd);
   }
 }
};

// 直连 session_node 的 ZMQ REQ 客户端（取消场景绕过同步转发的控制面）。
class ZmqReqClient {
public:
 explicit ZmqReqClient(zmq::context_t& ctx) : socket_(ctx, zmq::socket_type::req) {
   socket_.set(zmq::sockopt::sndtimeo, 3000);
   socket_.set(zmq::sockopt::rcvtimeo, 3000);
   socket_.set(zmq::sockopt::linger, 0);
 }

 void connect(const std::string& endpoint) { socket_.connect(endpoint); }

 bool call(const MessageEnvelope& req, MessageEnvelope& reply,
           std::chrono::milliseconds timeout) {
   socket_.set(zmq::sockopt::rcvtimeo, static_cast<int>(timeout.count()));
   try {
     socket_.send(zmq::buffer(req.to_json()), zmq::send_flags::none);
     zmq::message_t msg;
     const bool got = socket_.recv(msg, zmq::recv_flags::none).has_value();
     if (!got) {
       return false;
     }
     reply = MessageEnvelope::from_json(msg.to_string());
     return true;
   } catch (...) {
     return false;
   }
 }

private:
 zmq::socket_t socket_;
};

// 子进程句柄：spawn / SIGTERM / 带超时等待退出码。
struct ChildProc {
 pid_t pid = -1;

 bool spawn(const std::string& exe, const std::vector<std::string>& args,
            const std::string& log_path = "") {
   pid = fork();
   if (pid < 0) {
     return false;
   }
   if (pid == 0) {
     if (!log_path.empty()) {
       const int fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
       if (fd >= 0) {
         ::dup2(fd, STDOUT_FILENO);
         ::dup2(fd, STDERR_FILENO);
         ::close(fd);
       }
     }
     std::vector<char*> argv;
     argv.reserve(args.size() + 1);
     for (const auto& a : args) {
       argv.push_back(const_cast<char*>(a.c_str()));
     }
     argv.push_back(nullptr);
     ::execv(exe.c_str(), argv.data());
     ::_exit(127);
   }
   return true;
 }

 // 子进程是否存活（非阻塞）。
 bool alive() const {
   if (pid <= 0) {
     return false;
   }
   int status = 0;
   const pid_t r = ::waitpid(pid, &status, WNOHANG);
   return r == 0;
 }

 void kill(int sig = SIGTERM) {
   if (pid > 0) {
     ::kill(pid, sig);
   }
 }

 bool wait_for(std::chrono::milliseconds timeout, int* exit_code = nullptr) {
   if (pid <= 0) {
     return false;
   }
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (std::chrono::steady_clock::now() < deadline) {
     int status = 0;
     const pid_t r = ::waitpid(pid, &status, WNOHANG);
     if (r == pid) {
       if (exit_code != nullptr) {
         *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
       }
       return true;
     }
     std::this_thread::sleep_for(10ms);
   }
   return false;
 }
};

// 读取文件全部内容（子进程日志转储用）。
std::string ReadFile(const std::string& path) {
 std::FILE* f = std::fopen(path.c_str(), "rb");
 if (f == nullptr) {
   return "(无日志)";
 }
 std::string out;
 char buf[1024];
 std::size_t n = 0;
 while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
   out.append(buf, n);
 }
 std::fclose(f);
 return out;
}

MessageEnvelope MakeRequest(MessageType type, const std::string& work_id,
                           const std::string& request_id,
                           nlohmann::json payload = nlohmann::json::object()) {
 MessageEnvelope e;
 e.set_type(type);
 e.set_work_id(work_id);
 e.set_request_id(request_id);
 e.set_payload(std::move(payload));
 return e;
}

// 通过 TCP 网关发一条请求并解析响应。
bool exchange(TestClient& c, const MessageEnvelope& req, MessageEnvelope& reply,
             std::chrono::milliseconds timeout) {
 if (!c.send_all(req.to_json() + "\n")) {
   return false;
 }
 std::string line;
 if (!c.recv_line(line, timeout)) {
   return false;
 }
 try {
   reply = MessageEnvelope::from_json(line);
   return true;
 } catch (...) {
   return false;
 }
}

constexpr std::uint16_t kGatewayPort = 9101;  // 与 fake_nodes_e2e_test 错开
constexpr const char* kSessionNodeExe = "../../apps/session_node/session_node";
constexpr const char* kUnitManagerExe = "../../apps/unit_manager/unit_manager";
constexpr const char* kEdgeGatewayExe = "../../apps/edge_gateway/edge_gateway";

// e2e_dir：测试二进制目录（build-wsl/tests/e2e，绝对路径）；
// root：仓库根（绝对路径）。二者由 main 在 chdir 前计算。
void test_session_e2e(const std::string& e2e_dir, const std::string& root) {
 // 1. 拉起三进程：session_node → manager → gateway。
 const std::string apps = e2e_dir + "/../../apps";
 const std::string out_dir = e2e_dir + "/session-e2e-out";
 std::filesystem::remove_all(out_dir);
 const std::string log_dir = out_dir + "/logs";
 std::filesystem::create_directories(log_dir);
 ChildProc session_node, manager, gateway;
 CHECK(session_node.spawn(apps + "/session_node/session_node",
                          {"session_node",
                           "--listen", "tcp://127.0.0.1:19210",
                           "--config", root + "/config/mock/session.json",
                           "--output-dir", out_dir,
                           "--fixture-dir", root + "/data/fixtures",
                           "--stage-delay-ms", "20"},
                          log_dir + "/session_node.log"));
 CHECK(manager.spawn(apps + "/unit_manager/unit_manager",
                     {"unit_manager", "--node", "tcp://127.0.0.1:19210"},
                     log_dir + "/manager.log"));
 CHECK(gateway.spawn(apps + "/edge_gateway/edge_gateway",
                     {"edge_gateway", "--port", "9101"},
                     log_dir + "/gateway.log"));
 // 确认三进程存活（启动失败立即暴露，避免后续连锁失败难定位）。
 std::this_thread::sleep_for(500ms);
 if (!session_node.alive() || !manager.alive() || !gateway.alive()) {
   std::cerr << "子进程启动失败: session_node=" << session_node.alive()
             << " manager=" << manager.alive() << " gateway=" << gateway.alive()
             << std::endl;
   std::cerr << "--- session_node.log ---" << std::endl;
   std::cerr << ReadFile(log_dir + "/session_node.log") << std::endl;
   std::cerr << "--- manager.log ---" << std::endl;
   std::cerr << ReadFile(log_dir + "/manager.log") << std::endl;
   std::cerr << "--- gateway.log ---" << std::endl;
   std::cerr << ReadFile(log_dir + "/gateway.log") << std::endl;
 }
 TestClient c;
 CHECK(wait_until([&c] { return c.connect_to(kGatewayPort); }, 5000ms));

 // 2. setup：分配会话 work_id。
 MessageEnvelope reply;
 CHECK(exchange(c, MakeRequest(MessageType::kSetup, "", "s-1"), reply, 3000ms));
 CHECK(reply.type() == MessageType::kAck);
 CHECK(reply.work_id() == "w-0");
 std::cout << "  [ok] setup：会话 work_id 分配" << std::endl;

 // 3. 四类路由走对路径（经完整 TCP 链）。
 const char* kAudioAnswer =
     "SlotNexus 音频格式统一为 16kHz 单声道 16-bit PCM，帧长 20 毫秒";

 CHECK(exchange(c, MakeRequest(MessageType::kInference, "w-0", "r-l0",
                               {{"mode", "text"}, {"text", "停止播放"}}),
                reply, 3000ms));
 CHECK(reply.type() == MessageType::kAck);
 CHECK(reply.payload().value("route", std::string()) == "l0");
 CHECK(reply.payload().value("token_count", 1) == 0);  // 绕过 LLM
 CHECK(reply.payload().value("status", std::string()) == "ok");

 CHECK(exchange(c, MakeRequest(MessageType::kInference, "w-0", "r-l1",
                               {{"mode", "text"}, {"text", "16kHz 音频格式"}}),
                reply, 3000ms));
 CHECK(reply.payload().value("route", std::string()) == "l1");
 CHECK(reply.payload().value("token_count", 1) == 0);
 CHECK(reply.payload().value("final_text", std::string()) == kAudioAnswer);
 CHECK(reply.payload().value("pcm_frames", 0) > 0);

 CHECK(exchange(c, MakeRequest(MessageType::kInference, "w-0", "r-l2",
                               {{"mode", "text"}, {"text", "生成过滤怎么实现"}}),
                reply, 3000ms));
 CHECK(reply.payload().value("route", std::string()) == "l2");
 CHECK(reply.payload().value("token_count", 0) > 0);  // 走了 LLM
 {
   const std::string ft = reply.payload().value("final_text", std::string());
   CHECK(ft.find("Node Runtime 统一处理") != std::string::npos);  // 注入上下文
   CHECK(ft.find("生成过滤怎么实现") != std::string::npos);
 }

 CHECK(exchange(c, MakeRequest(MessageType::kInference, "w-0", "r-l3",
                               {{"mode", "text"}, {"text", "你好 今天天气怎么样"}}),
                reply, 3000ms));
 CHECK(reply.payload().value("route", std::string()) == "l3");
 CHECK(reply.payload().value("token_count", 0) > 0);
 CHECK(reply.payload().value("final_text", std::string()) == "你好 今天天气怎么样");
 std::cout << "  [ok] 四类路由：l0/l1/l2/l3 均走对路径（经完整链）" << std::endl;

 // 4. 固定 WAV 完整链路：voice.wav → ASR → 路由 → TTS → WAV 输出。
 CHECK(exchange(c, MakeRequest(MessageType::kInference, "w-0", "r-wav",
                               {{"mode", "wav"}, {"wav", "voice.wav"}}),
                reply, 3000ms));
 CHECK(reply.type() == MessageType::kAck);
 CHECK(reply.payload().value("status", std::string()) == "ok");
 CHECK(reply.payload().value("pcm_frames", 0) > 0);
 const std::string wav_asr_text =
     reply.payload().value("asr_text", std::string());
 CHECK(wav_asr_text.find("第1帧(320)") != std::string::npos);
 CHECK(wav_asr_text.find("第50帧(320)") != std::string::npos);
 {
   const std::string wav_path = reply.payload().value("wav_path", std::string());
   CHECK(!wav_path.empty());
   std::FILE* f = std::fopen(wav_path.c_str(), "rb");
   CHECK(f != nullptr);
   if (f != nullptr) {
     char magic[4];
     const std::size_t n = std::fread(magic, 1, 4, f);
     std::fclose(f);
     CHECK(n == 4 && std::string(magic, 4) == "RIFF");
   }
 }
 CHECK(exchange(c, MakeRequest(MessageType::kTaskInfo, "w-0", "t-wav"),
                reply, 3000ms));
 CHECK(reply.type() == MessageType::kAck);
 CHECK(reply.payload().value("asr_text", std::string()) == wav_asr_text);
 std::cout << "  [ok] 固定 WAV：ACK/taskinfo 返回 ASR 文本并输出 RIFF WAV 文件"
           << std::endl;

 // 5. 取消传播：直连 session_node（控制面同步转发的已知限制见测试头注释）。
 //   推理在工作线程上运行约 430ms（21 token × stage-delay 20ms），
 //   150ms 时另一条 REQ 连接发送 cancel，验证在途取消生效。
 zmq::context_t ctx(1);
 ZmqReqClient ca(ctx), cb(ctx);
 ca.connect("tcp://127.0.0.1:19210");
 cb.connect("tcp://127.0.0.1:19210");
 MessageEnvelope r_cancel_me;
 std::atomic<bool> infer_done{false};
 std::thread infer_thread([&] {
   ca.call(MakeRequest(MessageType::kInference, "w-0", "r-cancel-me",
                       {{"mode", "text"},
                        {"text",
                         "你好 今天天气怎么样 明天呢 后天呢 开心 快乐 轻松 愉快 "
                         "阳光 微风 散步 唱歌 跳舞 画画 读书 写字 下棋 钓鱼 "
                         "爬山 游泳"}}),
           r_cancel_me, 5000ms);
   infer_done.store(true);
 });
 std::this_thread::sleep_for(150ms);  // 已落入 LLM 生成阶段
 MessageEnvelope r_cancel;
 CHECK(cb.call(MakeRequest(MessageType::kCancel, "w-0", "r-cancel"),
               r_cancel, 3000ms));
 CHECK(r_cancel.type() == MessageType::kAck);
 infer_thread.join();
 CHECK(infer_done.load());
 CHECK(r_cancel_me.type() == MessageType::kAck);
 CHECK(r_cancel_me.payload().value("status", std::string()) == "cancelled");
 CHECK(r_cancel_me.payload().value("token_count", 0) > 0);  // 只接受取消前 token
 CHECK(r_cancel_me.payload().value("token_count", 999) < 21);
 CHECK(r_cancel_me.payload().value("pcm_frames", 999) == 0);  // 晚到数据 0 输出
 std::cout << "  [ok] 取消传播：在途推理被取消，晚到 token/PCM 全部过滤"
           << std::endl;

 // 新请求验证世代隔离：取消后旧数据不进入新请求。
 ZmqReqClient ca2(ctx);
 ca2.connect("tcp://127.0.0.1:19210");
 MessageEnvelope r_new;
 CHECK(ca2.call(MakeRequest(MessageType::kInference, "w-0", "r-after",
                            {{"mode", "text"}, {"text", "16kHz 音频格式"}}),
                r_new, 3000ms));
 CHECK(r_new.payload().value("status", std::string()) == "ok");
 CHECK(r_new.payload().value("route", std::string()) == "l1");
 CHECK(r_new.payload().value("final_text", std::string()) == kAudioAnswer);
 CHECK(r_new.payload().value("generation", 0) > 1);  // 取消 + 新请求已递增世代
 std::cout << "  [ok] 取消后新请求正常完成（世代隔离，直连 session_node）"
           << std::endl;

 // 6. taskinfo 与 exit（经完整链）。
 CHECK(exchange(c, MakeRequest(MessageType::kTaskInfo, "w-0", "t-1"), reply,
                3000ms));
 CHECK(reply.type() == MessageType::kAck);
 CHECK(reply.payload().value("state", std::string()) == "idle");
 CHECK(reply.payload().value("busy", true) == false);
 CHECK(reply.payload().value("route", std::string()) == "l1");
 CHECK(reply.payload().value("text_queue_peak", 999) <= 8);
 CHECK(reply.payload().value("pcm_queue_peak", 999) <= 32);
 std::cout << "  [ok] taskinfo：会话状态与队列峰值（不超过容量）" << std::endl;

 CHECK(exchange(c, MakeRequest(MessageType::kExit, "w-0", "e-1"), reply, 3000ms));
 CHECK(reply.type() == MessageType::kAck);
 CHECK(exchange(c, MakeRequest(MessageType::kTaskInfo, "w-0", "t-2"), reply,
                3000ms));
 CHECK(reply.type() == MessageType::kError);  // 已释放
 std::cout << "  [ok] exit：任务释放后 taskinfo 返回 not_exist" << std::endl;

 // 7. SIGTERM：三进程全部优雅退出，退出码 0。
 gateway.kill();
 manager.kill();
 session_node.kill();
 int code = -1;
 CHECK(gateway.wait_for(5000ms, &code));
 CHECK(code == 0);
 CHECK(manager.wait_for(5000ms, &code));
 CHECK(code == 0);
 CHECK(session_node.wait_for(5000ms, &code));
 CHECK(code == 0);
 std::cout << "  [ok] SIGTERM 三进程全部优雅退出（退出码 0）" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
 // 测试二进制位于 build-wsl/tests/e2e/：先按旧 CWD 计算绝对路径，
 // 再切到仓库根（子进程继承的 CWD 使 config/data 相对路径可解析）。
 std::string base = argc > 0 ? argv[0] : ".";
 const auto slash = base.find_last_of('/');
 base = (slash == std::string::npos) ? "." : base.substr(0, slash);
 const std::string e2e_dir = std::filesystem::absolute(base).string();
 const std::string root = std::filesystem::absolute(base + "/../../..").string();
 ::chdir(root.c_str());
 std::cout << "session_e2e_test:" << std::endl;
 test_session_e2e(e2e_dir, root);

 if (g_failures == 0) {
   std::cout << "session_e2e_test 全部通过" << std::endl;
   return 0;
 }
 std::cerr << "session_e2e_test 失败 " << g_failures << " 项" << std::endl;
 return 1;
}
