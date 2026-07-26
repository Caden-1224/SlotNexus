// 链路级故障注入回归（Mock 冻结回归，M1 门禁 4）：
// 真实进程拓扑 client --TCP--> edge_gateway --ZMQ--> unit_manager
//                    --ZMQ--> session_node(19221)。
//
// 覆盖：
//   1. 非法 JSON：网关回结构化错误信封（kError），连接保持可用；
//   2. 超长帧（>1 MiB）：连接层协议错误直接关闭连接，网关进程存活；
//   3. 未知 work_id：全链路结构化错误（manager 未命中路由即拒绝），
//      三进程存活，随后合法请求恢复正常；
//   4. 重复 cancel/exit（经全链）：幂等应答，释放后可继续新建会话；
//   5. 错误输入后进程可正常退出：SIGTERM 三进程退出码 0，
//      /proc 无进程残留、/proc/net/tcp 无 LISTEN 残留（M1 门禁 2）。
//
// 端口约定：与 50 轮轮换回归（9112/19111/19211）及进程内故障测试
// （19220/19222）错开，避免跨测试 ZMQ 重连注入。
#include "voxorchestra/protocol/message_envelope.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace ep = voxorchestra::protocol;
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

bool wait_until(const std::function<bool()>& cond,
                std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) {
      return true;
    }
    std::this_thread::sleep_for(2ms);
  }
  return cond();
}

// TCP 客户端（与 e2e 测试相同，一次 recv 多行不丢失）。
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
    return ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                     sizeof(addr)) == 0;
  }

  bool send_all(const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
      const ssize_t n =
          ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
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

  bool spawn(const std::string& exe, const std::vector<std::string>& args,
             const std::string& log_path = "") {
    pid = fork();
    if (pid < 0) {
      return false;
    }
    if (pid == 0) {
      if (!log_path.empty()) {
        const int fd =
            ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
      if (r < 0 && errno == ECHILD) {
        // 已被 alive() 的 waitpid 收割：视为已退出，退出码不可知。
        if (exit_code != nullptr) {
          *exit_code = -1;
        }
        return true;
      }
      std::this_thread::sleep_for(10ms);
    }
    return false;
  }
};

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

// ---------- 残留检查（M1 门禁 2） ----------

// /proc 扫描指定 comm 的存活进程，返回 PID 串。
std::vector<std::string> RunningProcesses(const std::vector<std::string>& names) {
  std::vector<std::string> found;
  for (const auto& entry : std::filesystem::directory_iterator("/proc")) {
    const std::string pid = entry.path().filename().string();
    if (pid.empty() ||
        !std::all_of(pid.begin(), pid.end(),
                     [](char c) { return c >= '0' && c <= '9'; })) {
      continue;
    }
    std::ifstream comm(entry.path() / "comm");
    std::string line;
    if (comm && std::getline(comm, line)) {
      for (const auto& n : names) {
        if (line == n) {
          found.push_back(pid);
        }
      }
    }
  }
  return found;
}

// /proc/net/tcp 中仍在 LISTEN（st=0A）的端口（输入十六进制小写端口串）。
std::vector<std::string> ListeningPorts(const std::vector<std::string>& ports_hex) {
  std::vector<std::string> found;
  std::ifstream f("/proc/net/tcp");
  std::string line;
  std::getline(f, line);  // 表头
  while (std::getline(f, line)) {
    if (line.find(" 0A ") == std::string::npos) {
      continue;  // 非 LISTEN
    }
    const std::size_t c1 = line.find(':');
    if (c1 == std::string::npos) {
      continue;
    }
    const std::size_t c2 = line.find(':', c1 + 1);
    if (c2 == std::string::npos) {
      continue;
    }
    const std::size_t end = line.find_first_of(" \t", c2 + 1);
    const std::string port = line.substr(c2 + 1, end - c2 - 1);
    for (const auto& p : ports_hex) {
      if (port == p) {
        found.push_back(p);
      }
    }
  }
  return found;
}

// ---------- 用例 ----------

constexpr std::uint16_t kGatewayPort = 9121;
constexpr const char* kManagerListen = "tcp://127.0.0.1:19121";
constexpr const char* kSessionListen = "tcp://127.0.0.1:19221";

void test_chain_faults(const std::string& e2e_dir, const std::string& root) {
  const std::string apps = e2e_dir + "/../../apps";
  const std::string out_dir = e2e_dir + "/chain-fault-out";
  std::filesystem::remove_all(out_dir);
  const std::string log_dir = out_dir + "/logs";
  std::filesystem::create_directories(log_dir);

  // 1. 拉起三进程：session_node → manager → gateway。
  ChildProc session_node, manager, gateway;
  CHECK(session_node.spawn(apps + "/session_node/session_node",
                           {"session_node", "--listen", kSessionListen,
                            "--config", root + "/config/mock/session.json",
                            "--output-dir", out_dir,
                            "--fixture-dir", root + "/data/fixtures",
                            "--stage-delay-ms", "20"},
                           log_dir + "/session_node.log"));
  CHECK(manager.spawn(apps + "/unit_manager/unit_manager",
                      {"unit_manager", "--listen", kManagerListen,
                       "--node", kSessionListen},
                      log_dir + "/manager.log"));
  CHECK(gateway.spawn(apps + "/edge_gateway/edge_gateway",
                      {"edge_gateway", "--port", "9121",
                       "--manager-url", kManagerListen},
                      log_dir + "/gateway.log"));
  std::this_thread::sleep_for(500ms);
  if (!session_node.alive() || !manager.alive() || !gateway.alive()) {
    std::cerr << "子进程启动失败: session_node=" << session_node.alive()
              << " manager=" << manager.alive()
              << " gateway=" << gateway.alive() << std::endl;
    std::cerr << "--- session_node.log ---" << std::endl;
    std::cerr << ReadFile(log_dir + "/session_node.log") << std::endl;
    std::cerr << "--- manager.log ---" << std::endl;
    std::cerr << ReadFile(log_dir + "/manager.log") << std::endl;
    std::cerr << "--- gateway.log ---" << std::endl;
    std::cerr << ReadFile(log_dir + "/gateway.log") << std::endl;
  }
  TestClient c;
  CHECK(wait_until([&c] { return c.connect_to(kGatewayPort); }, 5000ms));

  // ---------- 2. 非法 JSON：结构化错误 + 连接保持 ----------
  {
    CHECK(c.send_all("这不是 JSON{{{[\n"));
    std::string line;
    CHECK(c.recv_line(line, 3000ms));
    MessageEnvelope reply;
    try {
      reply = MessageEnvelope::from_json(line);
    } catch (...) {
      ++g_failures;
      std::cerr << "FAIL 非法 JSON 应答不是合法信封: " << line << std::endl;
    }
    CHECK(reply.type() == MessageType::kError);
    CHECK(!reply.error().empty());
    // 网关日志记录 bad_json（门禁 3 关联）。
    CHECK(ReadFile(log_dir + "/gateway.log").find("bad_json") !=
          std::string::npos);
    // 连接保持可用：随后合法 setup 正常完成。
    CHECK(exchange(c, MakeRequest(MessageType::kSetup, "", "s-1"), reply,
                   3000ms));
    CHECK(reply.type() == MessageType::kAck);
    CHECK(!reply.work_id().empty());
  }
  std::cout << "  [ok] 非法 JSON：结构化错误回包，连接保持可用" << std::endl;

  // ---------- 3. 超长帧（>1 MiB）：连接关闭，网关存活 ----------
  {
    TestClient big;
    CHECK(big.connect_to(kGatewayPort));
    const std::string junk((1u << 20) + 64, 'x');  // 1 MiB + 64 B，无换行
    (void)big.send_all(junk);  // 服务器可能在收满前关闭（EPIPE 合法）
    // 帧超限被连接层判为协议错误并关闭连接：读不到任何应答。
    std::string line;
    CHECK(!big.recv_line(line, 3000ms));
    // 网关仍存活并服务新连接。
    TestClient fresh;
    CHECK(fresh.connect_to(kGatewayPort));
    MessageEnvelope reply;
    CHECK(exchange(fresh, MakeRequest(MessageType::kTaskInfo, "w-0", "t-1"),
                   reply, 3000ms));
    CHECK(reply.type() == MessageType::kAck);
  }
  std::cout << "  [ok] 超长帧：连接关闭，网关进程存活并继续服务" << std::endl;

  // ---------- 4. 未知 work_id：全链路结构化错误，进程存活并恢复 ----------
  {
    MessageEnvelope reply;
    const MessageType types[] = {MessageType::kInference, MessageType::kCancel,
                                 MessageType::kTaskInfo, MessageType::kExit};
    for (const auto t : types) {
      CHECK(exchange(c, MakeRequest(t, "w-ghost", "g-1",
                                    {{"mode", "text"}, {"text", "x"}}),
                     reply, 3000ms));
      CHECK(reply.type() == MessageType::kError);
      CHECK(reply.error().message.find("未知任务") != std::string::npos);
    }
    // manager 日志记录 unknown_work_id（门禁 3 关联）。
    CHECK(ReadFile(log_dir + "/manager.log").find("unknown_work_id") !=
          std::string::npos);
    // 三进程均存活（结构化错误而非崩溃）。
    CHECK(session_node.alive());
    CHECK(manager.alive());
    CHECK(gateway.alive());
    // 恢复：合法 setup + inference 全链正常。
    CHECK(exchange(c, MakeRequest(MessageType::kSetup, "", "s-2"), reply,
                   3000ms));
    CHECK(reply.type() == MessageType::kAck);
    CHECK(exchange(c, MakeRequest(MessageType::kInference, reply.work_id(),
                                  "r-1",
                                  {{"mode", "text"}, {"text", "16kHz 音频格式"}}),
                   reply, 3000ms));
    CHECK(reply.payload().value("status", std::string()) == "ok");
  }
  std::cout << "  [ok] 未知 work_id：全链结构化错误，进程存活并恢复"
            << std::endl;

  // ---------- 5. 重复 cancel / exit（经全链） ----------
  {
    MessageEnvelope reply;
    CHECK(exchange(c, MakeRequest(MessageType::kSetup, "", "s-3"), reply,
                   3000ms));
    CHECK(reply.type() == MessageType::kAck);
    const std::string w = reply.work_id();
    // 重复 cancel：均 ack（幂等）。
    CHECK(exchange(c, MakeRequest(MessageType::kCancel, w, "c-1"), reply,
                   3000ms));
    CHECK(reply.type() == MessageType::kAck);
    CHECK(exchange(c, MakeRequest(MessageType::kCancel, w, "c-2"), reply,
                   3000ms));
    CHECK(reply.type() == MessageType::kAck);
    // 重复 exit：首次 ack；再次结构化错误（manager 路由已释放）。
    CHECK(exchange(c, MakeRequest(MessageType::kExit, w, "e-1"), reply,
                   3000ms));
    CHECK(reply.type() == MessageType::kAck);
    CHECK(exchange(c, MakeRequest(MessageType::kExit, w, "e-2"), reply,
                   3000ms));
    CHECK(reply.type() == MessageType::kError);
    CHECK(reply.error().message.find("未知任务") != std::string::npos);
    // 释放后可继续新建会话（manager 全局分配 work_id）。
    CHECK(exchange(c, MakeRequest(MessageType::kSetup, "", "s-4"), reply,
                   3000ms));
    CHECK(reply.type() == MessageType::kAck);
    CHECK(exchange(c, MakeRequest(MessageType::kExit, reply.work_id(), "e-3"),
                   reply, 3000ms));
    CHECK(reply.type() == MessageType::kAck);
  }
  std::cout << "  [ok] 重复 cancel/exit：幂等应答，释放后可新建会话"
            << std::endl;

  // ---------- 6. 错误输入后进程可正常退出（M1 门禁 2/4） ----------
  // 上述全部故障注入后 SIGTERM：三进程退出码 0，/proc 无残留进程、
  // /proc/net/tcp 无 LISTEN 残留。
  gateway.kill();
  manager.kill();
  session_node.kill();
  int code = -1;
  const bool gw_exit = gateway.wait_for(5000ms, &code);
  const int gw_code = code;
  code = -1;
  const bool mgr_exit = manager.wait_for(5000ms, &code);
  const int mgr_code = code;
  code = -1;
  const bool node_exit = session_node.wait_for(5000ms, &code);
  const int node_code = code;
  CHECK(gw_exit && gw_code == 0);
  CHECK(mgr_exit && mgr_code == 0);
  CHECK(node_exit && node_code == 0);

  const auto leftover =
      RunningProcesses({"session_node", "unit_manager", "edge_gateway"});
  CHECK(leftover.empty());
  const auto ports = ListeningPorts({"23a1", "4ab1", "4b15"});
  CHECK(ports.empty());

  std::cout << "PROCESS_CLEANUP" << std::endl;
  std::cout << "  gateway exit=" << gw_code << " manager exit=" << mgr_code
            << " session_node exit=" << node_code << std::endl;
  std::cout << "  leftover_processes=" << leftover.size()
            << " listening_ports=" << ports.size() << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
  // 测试二进制位于 build-wsl/tests/fault/：先按旧 CWD 计算绝对路径，
  // 再切到仓库根（子进程继承的 CWD 使 config/data 相对路径可解析）。
  std::string base = argc > 0 ? argv[0] : ".";
  const auto slash = base.find_last_of('/');
  base = (slash == std::string::npos) ? "." : base.substr(0, slash);
  const std::string e2e_dir = std::filesystem::absolute(base).string();
  const std::string root = std::filesystem::absolute(base + "/../../..").string();
  ::chdir(root.c_str());
  std::cout << "chain_fault_test:" << std::endl;
  test_chain_faults(e2e_dir, root);

  if (g_failures == 0) {
    std::cout << "chain_fault_test 全部通过（含进程清理）" << std::endl;
    return 0;
  }
  std::cerr << "chain_fault_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
