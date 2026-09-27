// 多 Fake 节点端到端集成测试（验收）：
// Author: Caden
//
// 真实进程拓扑：
//  echo/asr/rag/llm/tts 五节点 --ZMQ--> unit_manager <--ZMQ-- edge_gateway <--TCP-- 客户端
//  unit_manager 以轮转把 work_id 路由到五个节点（每个节点一个后端工厂）。
//
// 验收标准：
//  1. 五个 Backend（Echo + FakeAsr/FakeRetriever/FakeLlm/FakeTts）在同一
//     RuntimeNode 外壳下独立运行，Manager 路由互不串扰；
//  2. 每类 Fake 的确定性输出经全链路可精确断言；
//  3. tts 产出真实 WAV 文件（44 字节头 + PCM 数据）；
//  4. SIGTERM 后七个进程全部优雅退出（退出码 0）。
#include "test_paths.hpp"
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
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

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

// 与 echo_e2e_test 相同的测试客户端（一次 recv 多行不丢失）。
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

// 子进程句柄：spawn / SIGTERM / 带超时等待退出码。
struct ChildProc {
 pid_t pid = -1;

 bool spawn(const std::string& exe, const std::vector<std::string>& args) {
   pid = fork();
   if (pid < 0) {
     return false;
   }
   if (pid == 0) {
     std::vector<char*> argv;
     argv.reserve(args.size() + 1);
     for (const auto& a : args) {
       argv.push_back(const_cast<char*>(a.c_str()));
     }
     argv.push_back(nullptr);
     ::execv(exe.c_str(), argv.data());
     ::_exit(127);  // execv 失败
   }
   return true;
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

// 发一条请求并解析响应；失败返回 false。
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

constexpr std::uint16_t kGatewayPort = 9100;
constexpr const char* kEchoNodeExe = "../../apps/echo_node/echo_node";
constexpr const char* kAsrNodeExe = "../../apps/asr_node/asr_node";
constexpr const char* kRagNodeExe = "../../apps/rag_node/rag_node";
constexpr const char* kLlmNodeExe = "../../apps/llm_node/llm_node";
constexpr const char* kTtsNodeExe = "../../apps/tts_node/tts_node";
constexpr const char* kUnitManagerExe = "../../apps/unit_manager/unit_manager";
constexpr const char* kEdgeGatewayExe = "../../apps/edge_gateway/edge_gateway";

// 读取文件大小与开头字节（验证 WAV 文件）。
bool ReadFileHead(const std::string& path, std::vector<char>& head,
                 std::size_t* file_size) {
 std::FILE* f = std::fopen(path.c_str(), "rb");
 if (f == nullptr) {
   return false;
 }
 if (std::fseek(f, 0, SEEK_END) != 0) {
   std::fclose(f);
   return false;
 }
 const long size = std::ftell(f);
 if (file_size != nullptr) {
   *file_size = static_cast<std::size_t>(size);
 }
 std::rewind(f);
 head.resize(static_cast<std::size_t>(size > 4 ? 4 : size));
 const std::size_t n = std::fread(head.data(), 1, head.size(), f);
 std::fclose(f);
 return n == head.size();
}

void test_fake_nodes_e2e(const char* argv0) {
 // 测试二进制位于 <build>/tests/integration/，
 // apps 二进制位于其 ../../apps/<app>/<app>。
 std::string base = argv0;
 const auto slash = base.find_last_of('/');
 base = (slash == std::string::npos) ? "." : base.substr(0, slash);

 // 1. 拉起七个进程（顺序：五节点 → Manager → 网关）。
 //   各节点使用默认端口：echo 19200 / asr 19201 / rag 19202 / llm 19203 / tts 19204。
 ChildProc echo_node, asr_node, rag_node, llm_node, tts_node, manager, gateway;
 CHECK(echo_node.spawn(base + "/" + kEchoNodeExe, {"echo_node"}));
 CHECK(asr_node.spawn(base + "/" + kAsrNodeExe, {"asr_node"}));
 CHECK(rag_node.spawn(base + "/" + kRagNodeExe,
                      {"rag_node", "--knowledge",
                       vox_test::Root(argv0) + "/data/knowledge/knowledge.jsonl"}));
 CHECK(llm_node.spawn(base + "/" + kLlmNodeExe, {"llm_node"}));
 const std::string tts_dir = base + "/tts-e2e-out";
 std::filesystem::remove_all(tts_dir);
 CHECK(tts_node.spawn(base + "/" + kTtsNodeExe,
                      {"tts_node", "--output-dir", tts_dir}));
 CHECK(manager.spawn(base + "/" + kUnitManagerExe,
                     {"unit_manager", "--node", "tcp://127.0.0.1:19200",
                      "--node", "tcp://127.0.0.1:19201",
                      "--node", "tcp://127.0.0.1:19202",
                      "--node", "tcp://127.0.0.1:19203",
                      "--node", "tcp://127.0.0.1:19204"}));
 CHECK(gateway.spawn(base + "/" + kEdgeGatewayExe, {"edge_gateway"}));

 // 2. 等网关就绪并连上。
 TestClient c;
 CHECK(wait_until([&c] { return c.connect_to(kGatewayPort); }, 5000ms));

 // 3. 五个 setup 轮转路由：w-0→echo / w-1→asr / w-2→rag / w-3→llm / w-4→tts。
 MessageEnvelope reply;
 for (int i = 0; i < 5; ++i) {
   CHECK(exchange(c, MakeRequest(MessageType::kSetup, "", "s-" + std::to_string(i)),
                  reply, 3000ms));
   CHECK(reply.type() == MessageType::kAck);
   CHECK(reply.work_id() == "w-" + std::to_string(i));
 }
 std::cout << "  [ok] 五节点 setup 轮转路由：w-0..w-4 分别落在五个节点" << std::endl;

 // 4. 每个节点各发一次推理，验证确定性输出与 request_id 对应（无跨流）。
 CHECK(exchange(c, MakeRequest(MessageType::kInference, "w-0", "r-echo",
                               {{"text", "你好"}}),
                reply, 3000ms));
 CHECK(reply.type() == MessageType::kAck);
 CHECK(reply.request_id() == "r-echo");
 CHECK(reply.payload().value("text", std::string()) == "echo:你好");

 CHECK(exchange(c, MakeRequest(MessageType::kInference, "w-1", "r-asr",
                               {{"text", "3"}}),
                reply, 3000ms));
 CHECK(reply.type() == MessageType::kAck);
 CHECK(reply.request_id() == "r-asr");
 CHECK(reply.payload().value("text", std::string()) ==
       "第1帧(320) 第2帧(320) 第3帧(320)");

 CHECK(exchange(c, MakeRequest(MessageType::kInference, "w-2", "r-rag",
                               {{"text", "多进程架构怎么实现"}}),
                reply, 3000ms));
 CHECK(reply.type() == MessageType::kAck);
 CHECK(reply.request_id() == "r-rag");
 {
   // 真实 BM25 路由：L1 直答，Top-K=2 且得分降序（证据格式与级别一致）。
   const auto resp =
       nlohmann::json::parse(reply.payload().value("text", std::string()));
   CHECK(resp["level"] == "l1");
   CHECK(resp["chunks"].is_array() && resp["chunks"].size() == 2);
   CHECK(resp["chunks"][0]["id"] == "k-arch");
   CHECK(resp["chunks"][0]["score"].get<double>() >=
         resp["chunks"][1]["score"].get<double>());
   CHECK(!resp["answer"].get<std::string>().empty());
   CHECK(resp["prompt"].get<std::string>().empty());
 }

 CHECK(exchange(c, MakeRequest(MessageType::kInference, "w-3", "r-llm",
                               {{"text", "你好 世界 SlotNexus"}}),
                reply, 3000ms));
 CHECK(reply.type() == MessageType::kAck);
 CHECK(reply.request_id() == "r-llm");
 CHECK(reply.payload().value("text", std::string()) == "你好 世界 SlotNexus");

 CHECK(exchange(c, MakeRequest(MessageType::kInference, "w-4", "r-tts",
                               {{"text", "SlotNexus"}}),
                reply, 3000ms));
 CHECK(reply.type() == MessageType::kAck);
 CHECK(reply.request_id() == "r-tts");
 {
   const auto meta =
       nlohmann::json::parse(reply.payload().value("text", std::string()));
   CHECK(meta["pcm_bytes"] == 640);  // 8 字节文本 → 1 块 × 320 采样 × 2 字节
   const std::string wav_path = meta["wav_path"].get<std::string>();
   std::size_t file_size = 0;
   std::vector<char> head;
   CHECK(ReadFileHead(wav_path, head, &file_size));
   CHECK(file_size == 44 + 640);                          // 标准 WAV 头 + 数据
   CHECK(head.size() == 4 &&
         std::string(head.data(), head.size()) == "RIFF");  // WAV 魔数
 }
 std::cout << "  [ok] 五类后端独立输出：echo/asr/rag/llm/tts 均精确命中且无跨流"
           << std::endl;

 // 5. asr 任务通道复用：同任务第二次推理正常（状态机不受影响）。
 CHECK(exchange(c, MakeRequest(MessageType::kInference, "w-1", "r-asr-2",
                               {{"text", "1"}}),
                reply, 3000ms));
 CHECK(reply.type() == MessageType::kAck);
 CHECK(reply.payload().value("text", std::string()) == "第1帧(320)");
 CHECK(exchange(c, MakeRequest(MessageType::kTaskInfo, "w-1", "t-1"), reply, 3000ms));
 CHECK(reply.type() == MessageType::kAck);
 CHECK(reply.payload().value("inference_count", 0) == 2);
 std::cout << "  [ok] asr 通道复用：第二次推理与 taskinfo 计数正确" << std::endl;

 // 6. 未知任务：多节点路由下全链路返回 not_exist。
 CHECK(exchange(c, MakeRequest(MessageType::kInference, "w-999", "rx-1",
                               {{"text", "x"}}),
                reply, 3000ms));
 CHECK(reply.type() == MessageType::kError);
 CHECK(reply.error().code == 1);  // kNotExist
 std::cout << "  [ok] 未知任务经多节点路由返回 not_exist" << std::endl;

 // 7. SIGTERM：七个进程全部优雅退出，退出码 0。
 gateway.kill();
 manager.kill();
 echo_node.kill();
 asr_node.kill();
 rag_node.kill();
 llm_node.kill();
 tts_node.kill();
 int code = -1;
 CHECK(gateway.wait_for(5000ms, &code));
 CHECK(code == 0);
 CHECK(manager.wait_for(5000ms, &code));
 CHECK(code == 0);
 CHECK(echo_node.wait_for(5000ms, &code));
 CHECK(code == 0);
 CHECK(asr_node.wait_for(5000ms, &code));
 CHECK(code == 0);
 CHECK(rag_node.wait_for(5000ms, &code));
 CHECK(code == 0);
 CHECK(llm_node.wait_for(5000ms, &code));
 CHECK(code == 0);
 CHECK(tts_node.wait_for(5000ms, &code));
 CHECK(code == 0);
 std::cout << "  [ok] SIGTERM 后七个进程全部优雅退出（退出码 0）" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
 std::cout << "fake_nodes_e2e_test:" << std::endl;
 test_fake_nodes_e2e(argc > 0 ? argv[0] : "fake_nodes_e2e_test");

 if (g_failures == 0) {
   std::cout << "fake_nodes_e2e_test 全部通过" << std::endl;
   return 0;
 }
 std::cerr << "fake_nodes_e2e_test 失败 " << g_failures << " 项" << std::endl;
 return 1;
}
