// session_node 可执行入口：会话编排进程（编排中枢）。
//
// 用法：session_node [--listen tcp://127.0.0.1:19210]
//                   [--config config/mock/session.json]
//                   [--knowledge <jsonl>] [--output-dir <dir>]
//                   [--fixture-dir <dir>] [--direct-threshold <v>]
//                   [--context-threshold <v>] [--top-k <N>]
//                   [--text-capacity <N>] [--pcm-capacity <N>]
//                   [--push-timeout-ms <N>] [--stage-delay-ms <N>]
// 默认端口约定：echo 19200 / asr 19201 / rag 19202 / llm 19203 / tts 19204 /
//             session 19210。
//
// 进程拓扑：client -> TCP gateway -> unit_manager -> session_node(19210)。
// 每个 work_id 一个会话：Fake ASR/LLM/TTS + 真实 BM25 L0-L3 路由 +
// 有界队列 + generation 取消过滤；inference 在工作线程运行，期间
// cancel/taskinfo/exit 可并发到达。SIGINT/SIGTERM 优雅退出（退出码 0）。
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>
#include <zmq.hpp>

#include "session_node.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int /*sig*/) { g_stop = 1; }

// 读取 JSON 配置文件；读取失败返回 false 并输出原因。
bool load_config_json(const std::string& path, nlohmann::json& out) {
 std::ifstream in(path);
 if (!in) {
   std::cerr << "无法读取配置文件: " << path << std::endl;
   return false;
 }
 try {
   out = nlohmann::json::parse(in);
   return true;
 } catch (const nlohmann::json::exception& e) {
   std::cerr << "配置文件解析失败: " << e.what() << std::endl;
   return false;
 }
}

int parse_int(const char* s, int fallback) {
 try {
   return std::stoi(s);
 } catch (...) {
   return fallback;
 }
}

double parse_double(const char* s, double fallback) {
 try {
   return std::stod(s);
 } catch (...) {
   return fallback;
 }
}

}  // namespace

int main(int argc, char** argv) {
 voxorchestra::app::SessionNodeConfig config;

 // 先读配置文件（缺省路径），命令行参数随后覆盖。
 nlohmann::json file_cfg;
 std::string config_path = "config/mock/session.json";
 for (int i = 1; i < argc - 1; ++i) {
   if (std::string(argv[i]) == "--config") {
     config_path = argv[i + 1];
   }
 }
 if (load_config_json(config_path, file_cfg)) {
   if (file_cfg.contains("router")) {
     const auto& r = file_cfg["router"];
     if (r.contains("l0_keywords")) {
       config.router.l0_keywords =
           r["l0_keywords"].get<std::vector<std::string>>();
     }
     config.router.direct_threshold = r.value("direct_threshold",
                                              config.router.direct_threshold);
     config.router.context_threshold =
         r.value("context_threshold", config.router.context_threshold);
     config.router.top_k = r.value("top_k", static_cast<int>(config.router.top_k));
   }
   if (file_cfg.contains("knowledge")) {
     config.knowledge_path = file_cfg["knowledge"].get<std::string>();
   }
   if (file_cfg.contains("queues")) {
     const auto& q = file_cfg["queues"];
     config.text_capacity =
         q.value("text_capacity", static_cast<int>(config.text_capacity));
     config.pcm_capacity =
         q.value("pcm_capacity", static_cast<int>(config.pcm_capacity));
     config.push_timeout =
         std::chrono::milliseconds(q.value("push_timeout_ms", 50));
   }
   if (file_cfg.contains("output_dir")) {
     config.output_dir = file_cfg["output_dir"].get<std::string>();
   }
   config.stage_delay =
       std::chrono::milliseconds(file_cfg.value("stage_delay_ms", 0));
   config.tts_min_duration =
       std::chrono::milliseconds(file_cfg.value("tts_min_duration_ms", 0));
   config.max_run =
       std::chrono::milliseconds(file_cfg.value("max_run_ms", 30000));
 } else {
   return 1;
 }

 // 命令行覆盖。
 for (int i = 1; i < argc - 1; ++i) {
   const std::string arg = argv[i];
   const std::string val = argv[i + 1];
   if (arg == "--listen") {
     config.listen = val;
   } else if (arg == "--knowledge") {
     config.knowledge_path = val;
   } else if (arg == "--output-dir") {
     config.output_dir = val;
   } else if (arg == "--fixture-dir") {
     config.fixture_dir = val;
   } else if (arg == "--direct-threshold") {
     config.router.direct_threshold = parse_double(val.c_str(),
                                                   config.router.direct_threshold);
   } else if (arg == "--context-threshold") {
     config.router.context_threshold =
         parse_double(val.c_str(), config.router.context_threshold);
   } else if (arg == "--top-k") {
     config.router.top_k =
         static_cast<std::size_t>(parse_int(val.c_str(),
                                            static_cast<int>(config.router.top_k)));
   } else if (arg == "--text-capacity") {
     config.text_capacity =
         static_cast<std::size_t>(parse_int(val.c_str(),
                                            static_cast<int>(config.text_capacity)));
   } else if (arg == "--pcm-capacity") {
     config.pcm_capacity =
         static_cast<std::size_t>(parse_int(val.c_str(),
                                            static_cast<int>(config.pcm_capacity)));
   } else if (arg == "--push-timeout-ms") {
     config.push_timeout =
         std::chrono::milliseconds(parse_int(val.c_str(), 50));
   } else if (arg == "--stage-delay-ms") {
     config.stage_delay =
         std::chrono::milliseconds(parse_int(val.c_str(), 0));
   } else if (arg == "--tts-min-duration-ms") {
     config.tts_min_duration =
         std::chrono::milliseconds(parse_int(val.c_str(), 0));
   } else if (arg == "--max-run-ms") {
     config.max_run = std::chrono::milliseconds(parse_int(val.c_str(), 30000));
   }
 }

 std::signal(SIGINT, handle_signal);
 std::signal(SIGTERM, handle_signal);

 zmq::context_t ctx(1);
 voxorchestra::app::SessionNode node(ctx, config);
 try {
   node.bind();
   std::cout << "session_node 监听 " << config.listen << "（知识库 "
             << config.knowledge_path << "，direct="
             << config.router.direct_threshold << " context="
             << config.router.context_threshold << " top-k="
             << config.router.top_k << "，队列 " << config.text_capacity
             << "/" << config.pcm_capacity << "）" << std::endl;
 } catch (const std::exception& e) {
   std::cerr << "session_node 启动失败: " << e.what() << std::endl;
   return 1;
 }

 while (!g_stop) {
   node.serve_once(std::chrono::milliseconds(100));
 }
 node.close();
 std::cout << "session_node 已退出" << std::endl;
 return 0;
}
