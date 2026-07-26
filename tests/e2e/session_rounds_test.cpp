// Session 50 轮 Mock E2E 回归（M1 门禁 1/2/3）：
//
// 真实进程拓扑（与 session_e2e_test 相同）：
//   client --TCP(9112)--> edge_gateway --ZMQ(19111)--> unit_manager
//   --ZMQ(19211)--> session_node
//
// 端口刻意与其他测试错开（9112/19111/19211）：ZMQ REQ 客户端会向
// 端点自动重连，若本测试与先序测试共用端口，先序测试的遗留进程可能
// 把排队请求注入本测试链（TSan 回归中实测发现），唯一端口免疫该干扰。
//
// 在单条 TCP 连接上轮换四类路由 + 固定 WAV 共 50 轮，逐轮核对：
//   1. reply.request_id 必须等于本轮 request_id（跨流检测，门禁 1）；
//   2. reply.work_id 必须等于会话 work_id；
//   3. 路由与内容必须匹配本轮类型（L0 控制 / L1 直答 / L2 带上下文 /
//      L3 闲聊 / WAV 完整链路输出 RIFF 文件）；
// 统计：成功 / 超时 / 跨流 / 内容错配，M1 门禁 1 = 50 轮零跨流。
//
// 收尾（M1 门禁 2）：SIGTERM 三进程退出码 0；/proc 无进程残留；
// 9112/19111/19211 端口全部释放（/proc/net/tcp 无 LISTEN）。
//
// 日志关联（M1 门禁 3）：50 个 request_id 必须全部出现在
// gateway / manager / session_node 三份日志中。
//
// 用法：session_rounds_test [--summary <统计 JSON 路径>]
//   --summary 缺省写入测试输出目录（session-rounds-out/summary.json）。
#include "voxorchestra/protocol/message_envelope.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

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

// TCP 客户端（与 session_e2e_test 相同，一次 recv 多行不丢失）。
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

// 读取文件全部内容（子进程日志转储与关联检查用）。
std::string ReadFile(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    return "";
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

// ---------- 本轮类型：四类路由 + WAV 轮换 ----------

enum class Kind { kL0, kL1, kL2, kL3, kWav };
constexpr int kRounds = 50;
// 唯一端口（与其他测试错开，防 ZMQ 重连注入，见文件头注释）。
constexpr std::uint16_t kGatewayPort = 9112;

const char* KindName(Kind k) {
  switch (k) {
    case Kind::kL0: return "l0";
    case Kind::kL1: return "l1";
    case Kind::kL2: return "l2";
    case Kind::kL3: return "l3";
    case Kind::kWav: return "wav";
  }
  return "?";
}

Kind KindAt(int round) {
  static const Kind kSequence[] = {Kind::kL0, Kind::kL1, Kind::kL2, Kind::kL3,
                                   Kind::kWav};
  return kSequence[round % 5];
}

// 本轮请求负载（文本与 session_e2e_test 保持一致，结果确定性可断言）。
nlohmann::json RoundPayload(Kind k) {
  switch (k) {
    case Kind::kL0: return {{"mode", "text"}, {"text", "停止播放"}};
    case Kind::kL1: return {{"mode", "text"}, {"text", "16kHz 音频格式"}};
    case Kind::kL2: return {{"mode", "text"}, {"text", "生成过滤怎么实现"}};
    case Kind::kL3: return {{"mode", "text"}, {"text", "你好 今天天气怎么样"}};
    case Kind::kWav: return {{"mode", "wav"}, {"wav", "voice.wav"}};
  }
  return nlohmann::json::object();
}

// 按类型核对响应内容；不匹配时 detail 给出原因。
bool VerifyKind(Kind k, const MessageEnvelope& reply, std::string& detail) {
  const auto& p = reply.payload();
  const std::string status = p.value("status", std::string());
  const std::string route = p.value("route", std::string());
  if (reply.type() != MessageType::kAck) {
    detail = "type!=ack code=" + std::to_string(reply.error().code);
    return false;
  }
  if (status != "ok") {
    detail = "status=" + status;
    return false;
  }
  switch (k) {
    case Kind::kL0:
      if (route != "l0" || p.value("token_count", -1) != 0) {
        detail = "route=" + route + " tokens=" +
                 std::to_string(p.value("token_count", -1));
        return false;
      }
      return true;
    case Kind::kL1: {
      const std::string kAudioAnswer =
          "VoxOrchestra 音频格式统一为 16kHz 单声道 16-bit PCM，帧长 20 毫秒";
      if (route != "l1" || p.value("final_text", std::string()) != kAudioAnswer) {
        detail = "route=" + route;
        return false;
      }
      if (p.value("pcm_frames", -1) <= 0) {
        detail = "pcm_frames<=0";
        return false;
      }
      return true;
    }
    case Kind::kL2: {
      const std::string ft = p.value("final_text", std::string());
      if (route != "l2" || p.value("token_count", -1) <= 0 ||
          ft.find("Node Runtime 统一处理") == std::string::npos ||
          ft.find("生成过滤怎么实现") == std::string::npos) {
        detail = "route=" + route + " tokens=" +
                 std::to_string(p.value("token_count", -1));
        return false;
      }
      return true;
    }
    case Kind::kL3: {
      const std::string ft = p.value("final_text", std::string());
      if (route != "l3" || p.value("token_count", -1) <= 0 ||
          ft != "你好 今天天气怎么样") {
        detail = "route=" + route + " tokens=" +
                 std::to_string(p.value("token_count", -1));
        return false;
      }
      return true;
    }
    case Kind::kWav: {
      const std::string wav_path = p.value("wav_path", std::string());
      if (p.value("pcm_frames", -1) <= 0 || wav_path.empty()) {
        detail = "pcm_frames=" + std::to_string(p.value("pcm_frames", -1));
        return false;
      }
      std::FILE* f = std::fopen(wav_path.c_str(), "rb");
      if (f == nullptr) {
        detail = "wav 打不开: " + wav_path;
        return false;
      }
      char magic[4];
      const std::size_t n = std::fread(magic, 1, 4, f);
      std::fclose(f);
      if (n != 4 || std::string(magic, 4) != "RIFF") {
        detail = "wav 非 RIFF: " + wav_path;
        return false;
      }
      return true;
    }
  }
  return false;
}

// ---------- 残留检查（门禁 2） ----------

// /proc 扫描指定 comm 的存活进程，返回 PID 串。
std::vector<std::string> RunningProcesses(const std::vector<std::string>& names) {
  std::vector<std::string> found;
  for (const auto& entry : std::filesystem::directory_iterator("/proc")) {
    const std::string pid = entry.path().filename().string();
    if (pid.empty() || !std::all_of(pid.begin(), pid.end(),
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

// 日志文件是否包含指定 request_id（门禁 3 关联检查）。
bool LogContainsRequestId(const std::string& path, const std::string& request_id) {
  return ReadFile(path).find("request_id=" + request_id) != std::string::npos;
}

// ---------- 主流程 ----------

struct RoundStat {
  int success = 0;
  int timeout = 0;
  int cross_stream = 0;
  int mismatch = 0;
  std::vector<std::string> failures;  // 失败轮次明细
};

void test_50_rounds(const std::string& e2e_dir, const std::string& root,
                    const std::string& summary_path) {
  constexpr const char* kProcNames[] = {"session_node", "unit_manager",
                                        "edge_gateway"};
  // 端口十六进制：9112=0x2398 / 19111=0x4AA7 / 19211=0x4B0B。
  const std::vector<std::string> kPorts = {"2398", "4aa7", "4b0b"};

  // 0. 前置清理：避免上次失败运行残留进程占用端口（SIGTERM → 2s → SIGKILL）。
  std::string pre_cleanup = "none";
  for (int attempt = 0; attempt < 2; ++attempt) {
    const auto stale = RunningProcesses({"session_node", "unit_manager",
                                         "edge_gateway"});
    if (stale.empty()) {
      break;
    }
    pre_cleanup = "killed:" + std::to_string(stale.size());
    for (const auto& pid_str : stale) {
      const pid_t pid = std::stoi(pid_str);
      ::kill(pid, attempt == 0 ? SIGTERM : SIGKILL);
    }
    std::this_thread::sleep_for(1s);
  }
  if (pre_cleanup != "none") {
    std::cout << "  [pre] 残留进程清理完成: " << pre_cleanup << std::endl;
  }

  // 1. 拉起三进程：session_node → manager → gateway。
  const std::string apps = e2e_dir + "/../../apps";
  const std::string out_dir = e2e_dir + "/session-rounds-out";
  std::filesystem::remove_all(out_dir);
  const std::string log_dir = out_dir + "/logs";
  std::filesystem::create_directories(log_dir);
  ChildProc session_node, manager, gateway;
  CHECK(session_node.spawn(apps + "/session_node/session_node",
                           {"session_node",
                            "--listen", "tcp://127.0.0.1:19211",
                            "--config", root + "/config/mock/session.json",
                            "--output-dir", out_dir,
                            "--fixture-dir", root + "/data/fixtures",
                            "--stage-delay-ms", "10"},
                           log_dir + "/session_node.log"));
  CHECK(manager.spawn(apps + "/unit_manager/unit_manager",
                      {"unit_manager",
                       "--listen", "tcp://127.0.0.1:19111",
                       "--node", "tcp://127.0.0.1:19211"},
                      log_dir + "/manager.log"));
  CHECK(gateway.spawn(apps + "/edge_gateway/edge_gateway",
                      {"edge_gateway", "--port", "9112",
                       "--manager-url", "tcp://127.0.0.1:19111"},
                      log_dir + "/gateway.log"));
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
  CHECK(exchange(c, MakeRequest(MessageType::kSetup, "", "s-50"), reply, 3000ms));
  CHECK(reply.type() == MessageType::kAck);
  CHECK(reply.work_id() == "w-0");
  std::cout << "  [ok] setup：work_id=" << reply.work_id() << std::endl;

  // 3. 50 轮轮换：四类路由 + WAV。
  RoundStat stat;
  std::vector<bool> round_ok(kRounds, false);
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<std::string> request_ids;
  for (int i = 0; i < kRounds; ++i) {
    const Kind kind = KindAt(i);
    const std::string rid = "r50-" + std::to_string(i);
    request_ids.push_back(rid);
    MessageEnvelope r;
    const bool got = exchange(c, MakeRequest(MessageType::kInference, "w-0", rid,
                                             RoundPayload(kind)),
                              r, 5000ms);
    if (!got) {
      ++stat.timeout;
      stat.failures.push_back(rid + " kind=" + KindName(kind) + " 超时");
      std::cerr << "  [FAIL] round " << i << " " << KindName(kind) << " " << rid
                << " 超时" << std::endl;
      continue;
    }
    // 跨流检测：应答的 request_id/work_id 必须与本轮完全一致。
    if (r.request_id() != rid || r.work_id() != "w-0") {
      ++stat.cross_stream;
      stat.failures.push_back(rid + " kind=" + KindName(kind) + " 跨流 reply=" +
                              r.request_id() + " work=" + r.work_id());
      std::cerr << "  [FAIL] round " << i << " " << KindName(kind) << " " << rid
                << " 跨流: reply.request_id=" << r.request_id()
                << " work_id=" << r.work_id() << std::endl;
      continue;
    }
    std::string detail;
    if (!VerifyKind(kind, r, detail)) {
      ++stat.mismatch;
      stat.failures.push_back(rid + " kind=" + KindName(kind) + " 内容错配: " + detail);
      std::cerr << "  [FAIL] round " << i << " " << KindName(kind) << " " << rid
                << " 内容错配: " << detail << std::endl;
      continue;
    }
    round_ok[i] = true;
    ++stat.success;
    if ((i + 1) % 10 == 0) {
      std::cout << "  [ok] 已完成 " << (i + 1) << "/" << kRounds << " 轮（"
                << KindName(kind) << "）" << std::endl;
    }
  }
  const auto t1 = std::chrono::steady_clock::now();
  const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               t1 - t0).count();

  // 4. taskinfo：会话仍 idle（统计任务状态，不破坏会话）。
  CHECK(exchange(c, MakeRequest(MessageType::kTaskInfo, "w-0", "t-50"), reply,
                 3000ms));
  CHECK(reply.type() == MessageType::kAck);
  CHECK(reply.payload().value("state", std::string()) == "idle");
  CHECK(reply.payload().value("busy", true) == false);
  std::cout << "  [ok] taskinfo：50 轮后会话 idle、无在途请求" << std::endl;

  // 5. SIGTERM：三进程全部优雅退出（门禁 2 的退出码部分）。
  gateway.kill();
  manager.kill();
  session_node.kill();
  int code = -1;
  const bool gw_ok = gateway.wait_for(5000ms, &code);
  const int gw_code = code;
  const bool mgr_ok = manager.wait_for(5000ms, &code);
  const int mgr_code = code;
  const bool sn_ok = session_node.wait_for(5000ms, &code);
  const int sn_code = code;
  std::cout << "  [ok] SIGTERM 退出码: gateway=" << gw_code
            << " manager=" << mgr_code << " session_node=" << sn_code << std::endl;

  // 6. 进程/端口残留检查（门禁 2）。
  const auto leftover_procs = RunningProcesses(
      {"session_node", "unit_manager", "edge_gateway"});
  const auto leftover_ports = ListeningPorts(kPorts);
  std::cout << "  [ok] 残留进程=" << leftover_procs.size()
            << " 残留端口=" << leftover_ports.size() << std::endl;

  // 7. 日志关联（门禁 3）：50 个 request_id 三份日志全覆盖。
  int corr_gw = 0, corr_mgr = 0, corr_sn = 0;
  for (const auto& rid : request_ids) {
    if (LogContainsRequestId(log_dir + "/gateway.log", rid)) ++corr_gw;
    if (LogContainsRequestId(log_dir + "/manager.log", rid)) ++corr_mgr;
    if (LogContainsRequestId(log_dir + "/session_node.log", rid)) ++corr_sn;
  }
  std::cout << "  [ok] 日志关联: gateway=" << corr_gw << "/" << kRounds
            << " manager=" << corr_mgr << "/" << kRounds
            << " session_node=" << corr_sn << "/" << kRounds << std::endl;

  // 8. M1 门禁汇总并写统计 JSON。
  const bool gate_pass = stat.timeout == 0 && stat.cross_stream == 0 &&
                         stat.mismatch == 0 && leftover_procs.empty() &&
                         leftover_ports.empty() && gw_ok && mgr_ok && sn_ok &&
                         gw_code == 0 && mgr_code == 0 && sn_code == 0 &&
                         corr_gw == kRounds && corr_mgr == kRounds &&
                         corr_sn == kRounds;
  CHECK(gate_pass);

  nlohmann::json per_kind;
  for (int i = 0; i < kRounds; ++i) {
    // 按类型归类成功轮次（round_ok 为逐轮结果，避免 rid 前缀误匹配）。
    const Kind kind = KindAt(i);
    const std::string name = KindName(kind);
    if (!per_kind.contains(name)) {
      per_kind[name] = {{"total", 0}, {"success", 0}};
    }
    per_kind[name]["total"] = per_kind[name]["total"].get<int>() + 1;
    if (round_ok[i]) {
      per_kind[name]["success"] = per_kind[name]["success"].get<int>() + 1;
    }
  }

  nlohmann::json summary = {
      {"test", "session_rounds_test"},
      {"rounds_total", kRounds},
      {"success", stat.success},
      {"timeout", stat.timeout},
      {"cross_stream", stat.cross_stream},
      {"content_mismatch", stat.mismatch},
      {"duration_ms", duration_ms},
      {"per_kind", per_kind},
      {"log_correlation",
       {{"gateway", corr_gw}, {"manager", corr_mgr}, {"session_node", corr_sn}}},
      {"residue",
       {{"process_leftover", leftover_procs.size()},
        {"port_leftover", leftover_ports.size()},
        {"exit_codes", {gw_code, mgr_code, sn_code}}}},
      {"precondition_cleanup", pre_cleanup},
      {"gate_m1", gate_pass ? "pass" : "fail"}};
  if (!stat.failures.empty()) {
    summary["failures"] = stat.failures;
  }
  std::filesystem::path summary_file(summary_path);
  std::filesystem::create_directories(summary_file.parent_path());
  {
    std::ofstream out(summary_file);
    out << summary.dump(2) << std::endl;
  }
  std::cout << "summary: " << summary.dump() << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
  std::string summary_path;
  for (int i = 1; i < argc - 1; ++i) {
    if (std::string(argv[i]) == "--summary") {
      summary_path = argv[i + 1];
    }
  }
  // 测试二进制位于 build-wsl/tests/e2e/：先按旧 CWD 计算绝对路径，
  // 再切到仓库根（子进程继承的 CWD 使 config/data 相对路径可解析）。
  std::string base = argc > 0 ? argv[0] : ".";
  const auto slash = base.find_last_of('/');
  base = (slash == std::string::npos) ? "." : base.substr(0, slash);
  const std::string e2e_dir = std::filesystem::absolute(base).string();
  const std::string root = std::filesystem::absolute(base + "/../../..").string();
  ::chdir(root.c_str());
  if (summary_path.empty()) {
    summary_path = e2e_dir + "/session-rounds-out/summary.json";
  }
  std::cout << "session_rounds_test:" << std::endl;
  test_50_rounds(e2e_dir, root, summary_path);

  if (g_failures == 0) {
    std::cout << "session_rounds_test 全部通过（50 轮零跨流）" << std::endl;
    return 0;
  }
  std::cerr << "session_rounds_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
