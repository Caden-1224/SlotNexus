// 节点数据面事件发布集成测试（数据面落地）：
// Author: Caden
//
// 真实进程拓扑：
//  测试进程 --REQ/REP--> asr/llm/tts 三节点（fake 后端，--events 开启事件出口）
//  测试进程 --SUB-----> 各节点事件端点（主题 <work_id>/<request_id>/）
//
// 验收标准：
//  1. asr 推理过程 partial 逐帧实时发布，final 收尾（3 帧 → 3 partial + 1 final）；
//  2. llm 推理 token 实时发布，done 收尾（finish=true）；
//  3. tts 合成 PCM 帧实时发布（640 字节/帧），done 收尾；
//  4. 事件流按 work_id/request_id 主题隔离，三条流互不串扰；
//  5. SIGTERM 后三节点全部优雅退出（退出码 0）。
#include "slotnexus/dataplane/event_channel.hpp"
#include "slotnexus/protocol/message_envelope.hpp"
#include "slotnexus/transport/rpc.hpp"
#include "slotnexus/voice/event_adapter.hpp"

#include <chrono>
#include <csignal>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include <zmq.hpp>

using namespace std::chrono_literals;

namespace vd = slotnexus::dataplane;
namespace et = slotnexus::transport;
namespace ep = slotnexus::protocol;

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
   std::this_thread::sleep_for(5ms);
 }
 return cond();
}

// 子进程句柄（与 fake_nodes_e2e_test 相同的 spawn/清理模式）。
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

ep::MessageEnvelope MakeRequest(ep::MessageType type, const std::string& work_id,
                               const std::string& request_id,
                               nlohmann::json payload = nlohmann::json::object()) {
 ep::MessageEnvelope e;
 e.set_type(type);
 e.set_work_id(work_id);
 e.set_request_id(request_id);
 e.set_payload(std::move(payload));
 return e;
}

// 经 RPC 发 setup + inference 并断言 ack（节点未就绪时 REQ 排队等待，无需轮询）。
// 注意：直连节点时 work_id 由客户端指定（节点 setup_with 不分配），
// 与经网关路径（Manager 分配 work_id）行为一致。
void SetupAndInfer(et::RpcClient& rpc, const std::string& work_id,
                  const std::string& setup_rid, const std::string& infer_rid,
                  const nlohmann::json& payload) {
 const std::string ack =
     rpc.call(MakeRequest(ep::MessageType::kSetup, work_id, setup_rid).to_json(),
              5000ms);
 CHECK(ep::MessageEnvelope::from_json(ack).type() == ep::MessageType::kAck);
 const std::string resp =
     rpc.call(MakeRequest(ep::MessageType::kInference, work_id, infer_rid, payload)
                  .to_json(),
              5000ms);
 CHECK(ep::MessageEnvelope::from_json(resp).type() == ep::MessageType::kAck);
}

// 收集一条事件流直到 finish（超时返回 false）。
bool CollectStream(vd::EventSubscriber& sub, std::vector<vd::DataplaneEvent>& out,
                  std::chrono::milliseconds timeout) {
 const auto deadline = std::chrono::steady_clock::now() + timeout;
 while (std::chrono::steady_clock::now() < deadline) {
   vd::DataplaneEvent e;
   if (!sub.recv(e, 200ms)) {
     continue;
   }
   out.push_back(std::move(e));
   if (out.back().finish) {
     return true;
   }
 }
 return false;
}

constexpr const char* kAsrNodeExe = "../../apps/asr_node/asr_node";
constexpr const char* kLlmNodeExe = "../../apps/llm_node/llm_node";
constexpr const char* kTtsNodeExe = "../../apps/tts_node/tts_node";

void test_node_events(const char* argv0) {
 std::string base = argv0;
 const auto slash = base.find_last_of('/');
 base = (slash == std::string::npos) ? "." : base.substr(0, slash);

 // 1. 拉起三节点（fake 后端 + 事件出口）。事件端点固定端口避免与 RPC 端口冲突。
 const std::string tts_dir = base + "/tts-events-out";
 std::filesystem::remove_all(tts_dir);
 ChildProc asr_node, llm_node, tts_node;
 CHECK(asr_node.spawn(base + "/" + kAsrNodeExe,
                      {"asr_node", "--events", "tcp://127.0.0.1:19211",
                       "--events-sync", "tcp://127.0.0.1:19221"}));
 CHECK(llm_node.spawn(base + "/" + kLlmNodeExe,
                      {"llm_node", "--events", "tcp://127.0.0.1:19212",
                       "--events-sync", "tcp://127.0.0.1:19222"}));
 CHECK(tts_node.spawn(base + "/" + kTtsNodeExe,
                      {"tts_node", "--output-dir", tts_dir,
                       "--events", "tcp://127.0.0.1:19213",
                       "--events-sync", "tcp://127.0.0.1:19223"}));

 // 2. 订阅三条事件流（先订阅再连接，避免订阅生效前的丢流）。
 zmq::context_t ctx(1);
 vd::EventSubscriber sub_asr(ctx), sub_llm(ctx), sub_tts(ctx);
 sub_asr.subscribe("w-a", "r-a1");
 sub_llm.subscribe("w-l", "r-l1");
 sub_tts.subscribe("w-t", "r-t1");
 sub_asr.connect("tcp://127.0.0.1:19211");
 sub_llm.connect("tcp://127.0.0.1:19212");
 sub_tts.connect("tcp://127.0.0.1:19213");
 sub_asr.notify_ready("tcp://127.0.0.1:19221");
 sub_llm.notify_ready("tcp://127.0.0.1:19222");
 sub_tts.notify_ready("tcp://127.0.0.1:19223");

 // 数据面契约：订阅先于发布。fake 推理瞬时完成，发布可能在订阅传播
 // （SUB 连接建立 + 订阅报文到达发布端）之前发生而丢流——推理前等待
 // 订阅传播完成（回环上通常 <10ms，300ms 为稳健余量）。
 std::this_thread::sleep_for(300ms);

 // 3. 直连各节点 RPC：setup 后发推理。事件端点 bind 前 READY 在 zmq
 //   层排队，无握手竞态。
 et::RpcClient rpc_asr(ctx), rpc_llm(ctx), rpc_tts(ctx);
 rpc_asr.connect("tcp://127.0.0.1:19201");
 rpc_llm.connect("tcp://127.0.0.1:19203");
 rpc_tts.connect("tcp://127.0.0.1:19204");
 SetupAndInfer(rpc_asr, "w-a", "s-a1", "r-a1", {{"text", "3"}});
 SetupAndInfer(rpc_llm, "w-l", "s-l1", "r-l1", {{"text", "你好"}});
 SetupAndInfer(rpc_tts, "w-t", "s-t1", "r-t1", {{"text", "SlotNexus"}});

 // 4. 收集三条流并断言（各流独立收集，主题过滤保证互不串扰）。
 std::vector<vd::DataplaneEvent> ev_asr, ev_llm, ev_tts;
 CHECK(CollectStream(sub_asr, ev_asr, 5000ms));
 CHECK(CollectStream(sub_llm, ev_llm, 5000ms));
 CHECK(CollectStream(sub_tts, ev_tts, 5000ms));
 if (g_failures > 0) {
   return;  // 流收集失败：跳过断言（避免访问空向量），清理由末尾统一进行
 }

 std::vector<slotnexus::backend::BackendEvent> be_asr, be_llm, be_tts;
 for (const auto& e : ev_asr) be_asr.push_back(slotnexus::voice::FromDataplaneEvent(e));
 for (const auto& e : ev_llm) be_llm.push_back(slotnexus::voice::FromDataplaneEvent(e));
 for (const auto& e : ev_tts) be_tts.push_back(slotnexus::voice::FromDataplaneEvent(e));

 // asr：3 帧 → 3 partial + 1 final（finish=true），index 从 0 递增。
 CHECK(ev_asr.size() == 4);
 for (int i = 0; i < 3; ++i) {
   CHECK(be_asr[i].kind == slotnexus::backend::BackendEvent::Kind::kPartial);
   CHECK(ev_asr[i].index == i);
   CHECK(!ev_asr[i].finish);
 }
 CHECK(be_asr[0].text == "第1帧(320)");
 CHECK(be_asr[3].kind == slotnexus::backend::BackendEvent::Kind::kFinal);
 CHECK(be_asr[3].text == "第1帧(320) 第2帧(320) 第3帧(320)");
 CHECK(ev_asr[3].index == 3 && ev_asr[3].finish);
 std::cout << "  [ok] asr 事件流：3 partial 实时发布 + final 收尾（finish=true）"
           << std::endl;

 // llm：token×N + done（done 收尾 finish=true；token 数不依赖）。
 CHECK(ev_llm.size() >= 2);
 for (std::size_t i = 0; i + 1 < ev_llm.size(); ++i) {
   CHECK(be_llm[i].kind == slotnexus::backend::BackendEvent::Kind::kToken);
   CHECK(!ev_llm[i].finish);
 }
 CHECK(be_llm.back().kind == slotnexus::backend::BackendEvent::Kind::kDone);
 CHECK(ev_llm.back().finish);
 std::cout << "  [ok] llm 事件流：token 实时发布 + done 收尾（finish=true）"
           << std::endl;

 // tts：1 pcm 帧（640 字节）+ done（finish=true）。
 CHECK(ev_tts.size() == 2);
 CHECK(be_tts[0].kind == slotnexus::backend::BackendEvent::Kind::kPcm);
 CHECK(be_tts[0].pcm.size() == 320);  // 320 采样（16-bit）
 CHECK(ev_tts[0].index == 0 && !ev_tts[0].finish);
 CHECK(be_tts[1].kind == slotnexus::backend::BackendEvent::Kind::kDone);
 CHECK(ev_tts[1].index == 1 && ev_tts[1].finish);
 std::cout << "  [ok] tts 事件流：PCM 帧（640 字节）实时发布 + done 收尾"
           << std::endl;

 // 5. SIGTERM：三节点全部优雅退出，退出码 0。
 asr_node.kill();
 llm_node.kill();
 tts_node.kill();
 int code = -1;
 CHECK(asr_node.wait_for(5000ms, &code));
 CHECK(code == 0);
 CHECK(llm_node.wait_for(5000ms, &code));
 CHECK(code == 0);
 CHECK(tts_node.wait_for(5000ms, &code));
 CHECK(code == 0);
 std::cout << "  [ok] SIGTERM 后三节点全部优雅退出（退出码 0）" << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
 std::cout << "node_events_test:" << std::endl;
 test_node_events(argc > 0 ? argv[0] : "node_events_test");

 if (g_failures == 0) {
   std::cout << "node_events_test 全部通过" << std::endl;
   return 0;
 }
 std::cerr << "node_events_test 失败 " << g_failures << " 项" << std::endl;
 return 1;
}
