// 数据面全链路端到端测试（数据面落地 7.3 验收）：
//
// 真实进程拓扑：
//  client --ZMQ REQ--> session_node(19310, --backend net)
//    session_node 内嵌网络后端（NetAsr/NetLlm/NetTtsBackend）
//      --RPC 上行--> asr/llm/tts 三节点（19271/19272/19273）
//      <--PUB/SUB 事件下行（19281/19282/19283，握手 19291/19292/19293）
// 模型推理全部发生在独立节点进程；会话侧只编排：BM25 路由、分句、
// 有界队列、generation 取消过滤（与 embedded 模式同一 Pipeline）。
//
// 验收标准：
//  1. 四类路由走对路径（L0 控制 / L1 直答 / L2 带上下文 / L3 闲聊），
//     token/PCM 事件经数据面实时回放（token_count/pcm_frames > 0 即证明
//     事件流完整——embedded 模式不产生这些事件）；
//  2. 固定 WAV 完整链路：voice.wav 经 asr 节点识别 → 路由 → tts 节点
//     合成 → 输出可验证的 WAV 文件（ASR final 事件缺失则链路必然失败）；
//  3. 取消传播：inference 在途时 cancel 生效，晚到 token/PCM 全部过滤，
//     cancel 后节点任务可复用（新请求正常完成，世代隔离）；
//  4. SIGTERM 四进程全部优雅退出（退出码 0）。
#include "voxorchestra/backend/i_asr_backend.hpp"
#include "voxorchestra/common/base64.hpp"
#include "voxorchestra/common/wav_reader.hpp"
#include "voxorchestra/protocol/message_envelope.hpp"

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

#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zmq.hpp>

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

// 子进程句柄：spawn / SIGTERM / 带超时等待退出码（与 session_e2e_test 相同）。
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

// 直连 session_node 的 ZMQ REQ 客户端（会话侧无同步转发限制，直连即可）。
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

constexpr const char* kSessionNodeExe = "../../apps/session_node/session_node";
constexpr const char* kAsrNodeExe = "../../apps/asr_node/asr_node";
constexpr const char* kLlmNodeExe = "../../apps/llm_node/llm_node";
constexpr const char* kTtsNodeExe = "../../apps/tts_node/tts_node";
constexpr const char* kSessionListen = "tcp://127.0.0.1:19310";

// 测试用独立端口段，与既有测试/默认端口约定错开：
//  节点 RPC：19271/19272/19273；数据面事件：19281/19282/19283；
//  订阅握手：19291/19292/19293。
constexpr const char* kAsrRpc = "tcp://127.0.0.1:19271";
constexpr const char* kLlmRpc = "tcp://127.0.0.1:19272";
constexpr const char* kTtsRpc = "tcp://127.0.0.1:19273";
constexpr const char* kAsrEvents = "tcp://127.0.0.1:19281";
constexpr const char* kLlmEvents = "tcp://127.0.0.1:19282";
constexpr const char* kTtsEvents = "tcp://127.0.0.1:19283";
constexpr const char* kAsrSync = "tcp://127.0.0.1:19291";
constexpr const char* kLlmSync = "tcp://127.0.0.1:19292";
constexpr const char* kTtsSync = "tcp://127.0.0.1:19293";

// e2e_dir：测试二进制目录（build-wsl/tests/e2e，绝对路径）；
// root：仓库根（绝对路径）。二者由 main 在 chdir 前计算。
void test_session_net_e2e(const std::string& e2e_dir, const std::string& root) {
 // 1. 拉起四进程：三节点（fake 后端 + 数据面事件出口）→ session_node。
 const std::string apps = e2e_dir + "/../../apps";
 const std::string out_dir = e2e_dir + "/session-net-e2e-out";
 std::filesystem::remove_all(out_dir);
 const std::string log_dir = out_dir + "/logs";
 std::filesystem::create_directories(log_dir);
 ChildProc asr_node, llm_node, tts_node, session_node;
 CHECK(asr_node.spawn(apps + "/asr_node/asr_node",
                      {"asr_node", "--listen", kAsrRpc,
                       "--events", kAsrEvents, "--events-sync", kAsrSync},
                      log_dir + "/asr_node.log"));
 CHECK(llm_node.spawn(apps + "/llm_node/llm_node",
                      {"llm_node", "--listen", kLlmRpc,
                       "--events", kLlmEvents, "--events-sync", kLlmSync},
                      log_dir + "/llm_node.log"));
 CHECK(tts_node.spawn(apps + "/tts_node/tts_node",
                      {"tts_node", "--listen", kTtsRpc,
                       "--output-dir", out_dir + "/tts-node",
                       "--events", kTtsEvents, "--events-sync", kTtsSync},
                      log_dir + "/tts_node.log"));
 CHECK(session_node.spawn(apps + "/session_node/session_node",
                          {"session_node", "--listen", kSessionListen,
                           "--backend", "net",
                           "--asr-endpoint", kAsrRpc, "--asr-events", kAsrEvents,
                           "--asr-events-sync", kAsrSync,
                           "--llm-endpoint", kLlmRpc, "--llm-events", kLlmEvents,
                           "--llm-events-sync", kLlmSync,
                           "--tts-endpoint", kTtsRpc, "--tts-events", kTtsEvents,
                           "--tts-events-sync", kTtsSync,
                           "--config", root + "/config/mock/session.json",
                           "--output-dir", out_dir,
                           "--fixture-dir", root + "/data/fixtures",
                           "--stage-delay-ms", "20"},
                          log_dir + "/session_node.log"));
 // 确认四进程存活（启动失败立即暴露，避免后续连锁失败难定位）。
 std::this_thread::sleep_for(500ms);
 if (!asr_node.alive() || !llm_node.alive() || !tts_node.alive() ||
     !session_node.alive()) {
   std::cerr << "子进程启动失败: asr=" << asr_node.alive()
             << " llm=" << llm_node.alive() << " tts=" << tts_node.alive()
             << " session=" << session_node.alive() << std::endl;
   for (const char* n : {"asr_node", "llm_node", "tts_node", "session_node"}) {
     std::cerr << "--- " << n << ".log ---" << std::endl;
     std::cerr << ReadFile(log_dir + "/" + n + ".log") << std::endl;
   }
 }

 zmq::context_t ctx(1);
 ZmqReqClient ca(ctx);
 ca.connect(kSessionListen);
 MessageEnvelope reply;

 // 2. setup：会话侧同步向三节点 setup（net 模式节点不可达时此处失败）。
 CHECK(ca.call(MakeRequest(MessageType::kSetup, "w-0", "s-1"), reply, 5000ms));
 CHECK(reply.type() == MessageType::kAck);
 std::cout << "  [ok] setup：会话建立 + 三节点同步 setup" << std::endl;

 // 3. 四类路由走对路径（模型推理全部经数据面节点）。
 const char* kAudioAnswer =
     "VoxOrchestra 音频格式统一为 16kHz 单声道 16-bit PCM，帧长 20 毫秒";

 CHECK(ca.call(MakeRequest(MessageType::kInference, "w-0", "r-l0",
                           {{"mode", "text"}, {"text", "停止播放"}}),
               reply, 5000ms));
 CHECK(reply.type() == MessageType::kAck);
 CHECK(reply.payload().value("route", std::string()) == "l0");
 CHECK(reply.payload().value("token_count", 1) == 0);  // 绕过 LLM
 CHECK(reply.payload().value("pcm_frames", 0) > 0);    // TTS 经节点合成
 CHECK(reply.payload().value("status", std::string()) == "ok");

 CHECK(ca.call(MakeRequest(MessageType::kInference, "w-0", "r-l1",
                           {{"mode", "text"}, {"text", "16kHz 音频格式"}}),
               reply, 5000ms));
 CHECK(reply.type() == MessageType::kAck);
 CHECK(reply.payload().value("route", std::string()) == "l1");
 CHECK(reply.payload().value("token_count", 1) == 0);
 CHECK(reply.payload().value("final_text", std::string()) == kAudioAnswer);
 CHECK(reply.payload().value("pcm_frames", 0) > 0);

 CHECK(ca.call(MakeRequest(MessageType::kInference, "w-0", "r-l2",
                           {{"mode", "text"}, {"text", "生成过滤怎么实现"}}),
               reply, 5000ms));
 CHECK(reply.type() == MessageType::kAck);
 CHECK(reply.payload().value("route", std::string()) == "l2");
 // token 只经数据面事件回放产生：>0 即证明 LLM 事件流完整。
 CHECK(reply.payload().value("token_count", 0) > 0);
 {
   const std::string ft = reply.payload().value("final_text", std::string());
   CHECK(ft.find("Node Runtime 统一处理") != std::string::npos);  // 注入上下文
   CHECK(ft.find("生成过滤怎么实现") != std::string::npos);
 }

 CHECK(ca.call(MakeRequest(MessageType::kInference, "w-0", "r-l3",
                           {{"mode", "text"}, {"text", "你好 今天天气怎么样"}}),
               reply, 5000ms));
 CHECK(reply.type() == MessageType::kAck);
 CHECK(reply.payload().value("route", std::string()) == "l3");
 CHECK(reply.payload().value("token_count", 0) > 0);
 CHECK(reply.payload().value("final_text", std::string()) == "你好 今天天气怎么样");
 std::cout << "  [ok] 四类路由：l0/l1/l2/l3 均走对路径（token/PCM 经数据面回放）"
           << std::endl;

 // 4. 固定 WAV 完整链路：voice.wav → asr 节点（帧数约定）→ 路由 → tts 节点。
 CHECK(ca.call(MakeRequest(MessageType::kInference, "w-0", "r-wav",
                           {{"mode", "wav"}, {"wav", "voice.wav"}}),
               reply, 5000ms));
 CHECK(reply.type() == MessageType::kAck);
 CHECK(reply.payload().value("status", std::string()) == "ok");
 CHECK(reply.payload().value("pcm_frames", 0) > 0);
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
 std::cout << "  [ok] 固定 WAV：asr 节点识别 → tts 节点合成 → RIFF 输出"
           << std::endl;

 // 5. 取消传播：inference 在途时 cancel 生效。stage-delay 20ms/事件：
 //   LLM token 回放约 21 个（~420ms），400ms 时 cancel，晚到 token 过滤。
 ZmqReqClient cb(ctx);
 cb.connect(kSessionListen);
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
 std::this_thread::sleep_for(400ms);  // 已落入 LLM 生成阶段（token 回放中）
 MessageEnvelope r_cancel;
 CHECK(cb.call(MakeRequest(MessageType::kCancel, "w-0", "r-cancel"),
               r_cancel, 3000ms));
 CHECK(r_cancel.type() == MessageType::kAck);
 infer_thread.join();
 CHECK(infer_done.load());
 CHECK(r_cancel_me.type() == MessageType::kAck);
 CHECK(r_cancel_me.payload().value("status", std::string()) == "cancelled");
 CHECK(r_cancel_me.payload().value("token_count", 999) < 21);  // 只接受取消前 token
 CHECK(r_cancel_me.payload().value("pcm_frames", 999) == 0);   // 晚到数据 0 输出
 std::cout << "  [ok] 取消传播：在途推理被取消，晚到 token/PCM 全部过滤"
           << std::endl;

 // 6. 取消后节点任务可复用：新请求正常完成（世代隔离 + REQ 状态机恢复）。
 ZmqReqClient ca2(ctx);
 ca2.connect(kSessionListen);
 MessageEnvelope r_new;
 CHECK(ca2.call(MakeRequest(MessageType::kInference, "w-0", "r-after",
                            {{"mode", "text"}, {"text", "16kHz 音频格式"}}),
                r_new, 5000ms));
 CHECK(r_new.type() == MessageType::kAck);
 CHECK(r_new.payload().value("status", std::string()) == "ok");
 CHECK(r_new.payload().value("route", std::string()) == "l1");
 CHECK(r_new.payload().value("final_text", std::string()) == kAudioAnswer);
 CHECK(r_new.payload().value("generation", 0) > 1);
 std::cout << "  [ok] 取消后新请求正常完成（节点任务复用 + 世代隔离）"
           << std::endl;

 // 7. exit：会话释放，节点任务清理。
 CHECK(ca2.call(MakeRequest(MessageType::kExit, "w-0", "e-1"), reply, 3000ms));
 CHECK(reply.type() == MessageType::kAck);
 CHECK(ca2.call(MakeRequest(MessageType::kTaskInfo, "w-0", "t-1"), reply, 3000ms));
 CHECK(reply.type() == MessageType::kError);  // 已释放
 std::cout << "  [ok] exit：任务释放后 taskinfo 返回 not_exist" << std::endl;

 // 8. SIGTERM：四进程全部优雅退出，退出码 0。
 session_node.kill();
 asr_node.kill();
 llm_node.kill();
 tts_node.kill();
 int code = -1;
 CHECK(session_node.wait_for(5000ms, &code));
 CHECK(code == 0);
 CHECK(asr_node.wait_for(5000ms, &code));
 CHECK(code == 0);
 CHECK(llm_node.wait_for(5000ms, &code));
 CHECK(code == 0);
 CHECK(tts_node.wait_for(5000ms, &code));
 CHECK(code == 0);
 std::cout << "  [ok] SIGTERM 四进程全部优雅退出（退出码 0）" << std::endl;
}

// 音频上行（ASR 真实负载）端到端测试：
//  1. 节点级：直连 asr_node（fake 后端）发 pcm64 负载（voice.wav 的
//     PCM），断言按帧数约定合成出精确文本（16000 采样 / 320 = 50 帧）
//     ——证明 base64 解码与帧解释正确；
//  2. 全链路：session_node --asr-uplink + 三 fake 节点，固定 WAV 链路
//     完整（会话侧累积 PCM 上行 → 节点识别 → 路由 → TTS 节点合成），
//     输出 WAV 可验证。
void test_session_net_uplink_e2e(const std::string& e2e_dir,
                                const std::string& root) {
 const std::string apps = e2e_dir + "/../../apps";
 const std::string out_dir = e2e_dir + "/session-net-uplink-out";
 std::filesystem::remove_all(out_dir);
 const std::string log_dir = out_dir + "/logs";
 std::filesystem::create_directories(log_dir);

 // 测试用独立端口段（与既有用例 192xx 错开）。
 constexpr const char* kUpSessionListen = "tcp://127.0.0.1:19410";
 constexpr const char* kUpAsrRpc = "tcp://127.0.0.1:19471";
 constexpr const char* kUpLlmRpc = "tcp://127.0.0.1:19472";
 constexpr const char* kUpTtsRpc = "tcp://127.0.0.1:19473";
 constexpr const char* kUpAsrEvents = "tcp://127.0.0.1:19481";
 constexpr const char* kUpLlmEvents = "tcp://127.0.0.1:19482";
 constexpr const char* kUpTtsEvents = "tcp://127.0.0.1:19483";
 constexpr const char* kUpAsrSync = "tcp://127.0.0.1:19491";
 constexpr const char* kUpLlmSync = "tcp://127.0.0.1:19492";
 constexpr const char* kUpTtsSync = "tcp://127.0.0.1:19493";

 // 固定 WAV 的 PCM → base64 上行负载（voice.wav：16 kHz/16bit/单声道，
 // 16000 采样 = 50 帧）。
 const auto wav =
     voxorchestra::common::WavReader::read(root + "/data/fixtures/voice.wav");
 CHECK(wav.ok);
 const std::uint8_t* raw =
     reinterpret_cast<const std::uint8_t*>(wav.info.samples.data());
 const std::string b64 = voxorchestra::common::base64_encode(
     raw, wav.info.samples.size() * sizeof(int16_t));
 const std::string pcm_payload =
     std::string(voxorchestra::backend::kAsrPcmPayloadPrefix) + b64;
 constexpr int kFrames = 50;  // 16000 采样 / 320 采样每帧
 std::string expected_asr;
 for (int i = 1; i <= kFrames; ++i) {
   if (i > 1) {
     expected_asr += " ";
   }
   expected_asr += "第" + std::to_string(i) + "帧(320)";
 }

 ChildProc asr_node, llm_node, tts_node, session_node;
 CHECK(asr_node.spawn(apps + "/asr_node/asr_node",
                      {"asr_node", "--listen", kUpAsrRpc,
                       "--events", kUpAsrEvents, "--events-sync", kUpAsrSync},
                      log_dir + "/asr_node.log"));
 CHECK(llm_node.spawn(apps + "/llm_node/llm_node",
                      {"llm_node", "--listen", kUpLlmRpc,
                       "--events", kUpLlmEvents, "--events-sync", kUpLlmSync},
                      log_dir + "/llm_node.log"));
 CHECK(tts_node.spawn(apps + "/tts_node/tts_node",
                      {"tts_node", "--listen", kUpTtsRpc,
                       "--output-dir", out_dir + "/tts-node",
                       "--events", kUpTtsEvents, "--events-sync", kUpTtsSync},
                      log_dir + "/tts_node.log"));
 CHECK(session_node.spawn(apps + "/session_node/session_node",
                          {"session_node", "--listen", kUpSessionListen,
                           "--backend", "net",
                           "--asr-uplink",
                           "--asr-endpoint", kUpAsrRpc, "--asr-events", kUpAsrEvents,
                           "--asr-events-sync", kUpAsrSync,
                           "--llm-endpoint", kUpLlmRpc, "--llm-events", kUpLlmEvents,
                           "--llm-events-sync", kUpLlmSync,
                           "--tts-endpoint", kUpTtsRpc, "--tts-events", kUpTtsEvents,
                           "--tts-events-sync", kUpTtsSync,
                           "--config", root + "/config/mock/session.json",
                           "--output-dir", out_dir,
                           "--fixture-dir", root + "/data/fixtures",
                           "--stage-delay-ms", "20"},
                          log_dir + "/session_node.log"));
 // 确认四进程存活（启动失败立即暴露，避免后续连锁失败难定位）。
 std::this_thread::sleep_for(500ms);
 if (!asr_node.alive() || !llm_node.alive() || !tts_node.alive() ||
     !session_node.alive()) {
   std::cerr << "子进程启动失败: asr=" << asr_node.alive()
             << " llm=" << llm_node.alive() << " tts=" << tts_node.alive()
             << " session=" << session_node.alive() << std::endl;
   for (const char* n : {"asr_node", "llm_node", "tts_node", "session_node"}) {
     std::cerr << "--- " << n << ".log ---" << std::endl;
     std::cerr << ReadFile(log_dir + "/" + n + ".log") << std::endl;
   }
 }

 zmq::context_t ctx(1);

 // 1. 节点级：直连 asr_node（fake），pcm64 负载 → 帧数约定文本精确匹配。
 {
   ZmqReqClient cn(ctx);
   cn.connect(kUpAsrRpc);
   MessageEnvelope reply;
   CHECK(cn.call(MakeRequest(MessageType::kSetup, "w-up", "s-up"), reply,
                 5000ms));
   CHECK(reply.type() == MessageType::kAck);
   CHECK(cn.call(MakeRequest(MessageType::kInference, "w-up", "r-up-1",
                             {{"text", pcm_payload}}),
                 reply, 5000ms));
   CHECK(reply.type() == MessageType::kAck);
   CHECK(reply.payload().value("text", std::string()) == expected_asr);
   CHECK(cn.call(MakeRequest(MessageType::kExit, "w-up", "e-up"), reply,
                 3000ms));
   CHECK(reply.type() == MessageType::kAck);
   std::cout << "  [ok] 节点级 pcm64 负载：base64 解码 + 帧解释精确匹配（"
             << kFrames << " 帧）" << std::endl;
 }

 // 2. 全链路：--asr-uplink 下固定 WAV → 会话累积 PCM 上行 → 节点识别
 //   → 路由 → tts 节点合成 → RIFF 输出。
 {
   ZmqReqClient ca(ctx);
   ca.connect(kUpSessionListen);
   MessageEnvelope reply;
   CHECK(ca.call(MakeRequest(MessageType::kSetup, "w-1", "s-1"), reply,
                 5000ms));
   CHECK(reply.type() == MessageType::kAck);
   CHECK(ca.call(MakeRequest(MessageType::kInference, "w-1", "r-wav-up",
                             {{"mode", "wav"}, {"wav", "voice.wav"}}),
                 reply, 15000ms));
   CHECK(reply.type() == MessageType::kAck);
   CHECK(reply.payload().value("status", std::string()) == "ok");
   CHECK(!reply.payload().value("final_text", std::string()).empty());
   CHECK(reply.payload().value("pcm_frames", 0) > 0);
   const std::string wav_path =
       reply.payload().value("wav_path", std::string());
   CHECK(!wav_path.empty());
   std::FILE* f = std::fopen(wav_path.c_str(), "rb");
   CHECK(f != nullptr);
   if (f != nullptr) {
     char hdr[4] = {};
     const std::size_t got = std::fread(hdr, 1, 4, f);
     CHECK(got == 4 && std::memcmp(hdr, "RIFF", 4) == 0);
     std::fclose(f);
   }
   CHECK(ca.call(MakeRequest(MessageType::kExit, "w-1", "e-1"), reply,
                 3000ms));
   CHECK(reply.type() == MessageType::kAck);
   std::cout << "  [ok] 全链路 --asr-uplink：固定 WAV 音频上行 → 节点识别"
             << " → 输出 RIFF" << std::endl;
 }

 // 3. SIGTERM 四进程全部优雅退出，退出码 0。
 session_node.kill();
 asr_node.kill();
 llm_node.kill();
 tts_node.kill();
 int code = -1;
 CHECK(session_node.wait_for(5000ms, &code));
 CHECK(code == 0);
 CHECK(asr_node.wait_for(5000ms, &code));
 CHECK(code == 0);
 CHECK(llm_node.wait_for(5000ms, &code));
 CHECK(code == 0);
 CHECK(tts_node.wait_for(5000ms, &code));
 CHECK(code == 0);
 std::cout << "  [ok] SIGTERM 四进程全部优雅退出（退出码 0）" << std::endl;
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
 std::cout << "session_net_e2e_test:" << std::endl;
 test_session_net_e2e(e2e_dir, root);
 test_session_net_uplink_e2e(e2e_dir, root);

 if (g_failures == 0) {
   std::cout << "session_net_e2e_test 全部通过" << std::endl;
   return 0;
 }
 std::cerr << "session_net_e2e_test 失败 " << g_failures << " 项" << std::endl;
 return 1;
}
