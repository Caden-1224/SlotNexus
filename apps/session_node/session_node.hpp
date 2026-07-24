// session_node：会话编排进程（编排中枢）。
//
// 职责：
//  - 每个 work_id 一个 SessionPipeline 实例（Fake 后端 + 真实 BM25 路由）；
//  - ROUTER 异步服务：inference 在工作线程上运行完整管线，期间同一
//    连接仍可收到 cancel/taskinfo/exit（REQ 客户端与 ROUTER 兼容）；
//  - 回复经 inproc 管道回到服务线程统一发送（ZMQ socket 单线程访问）；
//  - setup/cancel/taskinfo/exit 立即应答；inference 完成后由工作线程
//    应答（携带路由证据与队列统计）。
//
// 进程拓扑：client -> TCP gateway -> unit_manager -> session_node(19210)。
// 端口约定：echo 19200 / asr 19201 / rag 19202 / llm 19203 / tts 19204 /
//          session 19210。
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <zmq.hpp>

#include "voxorchestra/rag/router.hpp"
#include "voxorchestra/session/session_pipeline.hpp"

namespace voxorchestra::app {

// session_node 运行配置（由 config/mock/session.json 与命令行注入）。
struct SessionNodeConfig {
 std::string listen = "tcp://127.0.0.1:19210";
 std::string knowledge_path = "data/knowledge/knowledge.jsonl";
 std::string output_dir = "session-out";     // WAV 输出目录
 std::string fixture_dir = "data/fixtures";  // 固定 WAV 输入目录
 rag::RouterConfig router;                   // L0-L3 阈值与关键词
 std::size_t text_capacity = 8;              // 有界文本队列容量
 std::size_t pcm_capacity = 32;              // 有界 PCM 队列容量
 std::chrono::milliseconds push_timeout{50}; // 满队列等待，超时丢弃
 std::chrono::milliseconds stage_delay{0};   // 测试仪表：阶段人工延时
 std::chrono::milliseconds tts_min_duration{0};  // 最小合成时长（补静音）
 std::chrono::milliseconds max_run{30000};   // 单次推理兜底上限
};

class SessionNode {
public:
 explicit SessionNode(zmq::context_t& ctx, SessionNodeConfig config);
 ~SessionNode();

 SessionNode(const SessionNode&) = delete;
 SessionNode& operator=(const SessionNode&) = delete;

 void bind();  // 绑定 ROUTER 与回复管道

 // 轮询一次：处理至多一批请求/回复；无事件且超时返回 false。
 bool serve_once(std::chrono::milliseconds poll_timeout);

 // 取消全部在途推理，等待工作线程退出后关闭 socket（幂等）。
 void close();

 std::size_t session_count() const;

 // 本节点监听端点（进程内测试夹具用）。
 const std::string& listen_endpoint() const { return config_.listen; }

private:
 struct Session;  // 定义在 session_node.cpp（持有后端与管线）

 // 服务线程收到请求后分发；同步应答直接发送，inference 转工作线程。
 void handle_request(const std::string& identity,
                     const std::string& request_json);
 // 工作线程：运行管线、更新最近结果并投递回复（identity + 响应 JSON）。
 void run_inference(const std::string& identity, const std::string& work_id,
                    const std::string& request_id, std::shared_ptr<Session> s,
                    const session::PipelineInput& input,
                    std::chrono::milliseconds deadline);

 zmq::context_t& ctx_;
 SessionNodeConfig config_;
 std::unique_ptr<rag::Router> router_;        // BM25 L0-L3 路由（共享只读）
 std::unique_ptr<zmq::socket_t> router_socket_;  // ZMQ ROUTER：服务线程独占
 std::unique_ptr<zmq::socket_t> replies_;     // PULL：工作线程回复汇入
 std::string reply_endpoint_;

 mutable std::mutex sessions_mutex_;
 std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;

 mutable std::mutex workers_mutex_;
 std::vector<std::thread> workers_;
 bool closed_ = false;
};

}  // namespace voxorchestra::app
