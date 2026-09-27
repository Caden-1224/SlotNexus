// unit_manager 可执行入口。
//
// Author: Caden
// 用法：
//   unit_manager --module-config <modules.json> [--listen ...] [--node-rpc-timeout-ms N]
//   unit_manager --module-id voice --node <endpoint> [--node ...] ...   # 兼容旧脚本
//
// 配置格式（最小模块描述）：
//   {"default_module_id":"voice","modules":[{"module_id":"voice",
//     "protocol_version":1,"config":"config/taishanpi3m/session.json",
//     "nodes":["tcp://127.0.0.1:19210"]}]}
// 旧请求不携带 payload.module_id 时使用 default_module_id 兼容路由。
#include <csignal>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <zmq.hpp>

#include "unit_manager.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int /*sig*/) { g_stop = 1; }

bool LoadModuleConfig(const std::string& path,
                      std::vector<slotnexus::manager::ModuleDescriptor>* modules,
                      std::string* default_module_id) {
  try {
    std::ifstream in(path);
    if (!in) {
      std::cerr << "模块配置读取失败: " << path << std::endl;
      return false;
    }
    nlohmann::json root;
    in >> root;
    if (!root.is_object() || !root.contains("modules") ||
        !root["modules"].is_array()) {
      std::cerr << "模块配置缺少 modules 数组: " << path << std::endl;
      return false;
    }
    modules->clear();
    for (const auto& item : root["modules"]) {
      slotnexus::manager::ModuleDescriptor descriptor;
      descriptor.module_id = item.value("module_id", std::string());
      descriptor.protocol_version = item.value("protocol_version", 1);
      descriptor.config_path = item.value("config", std::string());
      if (item.contains("nodes") && item["nodes"].is_array()) {
        for (const auto& node : item["nodes"]) {
          if (node.is_string()) {
            descriptor.node_endpoints.push_back(node.get<std::string>());
          }
        }
      }
      modules->push_back(std::move(descriptor));
    }
    *default_module_id = root.value("default_module_id", std::string());
    return !modules->empty();
  } catch (const std::exception& e) {
    std::cerr << "模块配置解析失败: " << e.what() << std::endl;
    return false;
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string listen = "tcp://127.0.0.1:19100";
  std::string module_config;
  std::string default_module_id;
  std::string legacy_module_id = "default";
  std::vector<std::string> legacy_nodes;
  long node_rpc_timeout_ms = 3000;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--listen" && i + 1 < argc) {
      listen = argv[++i];
    } else if (arg == "--module-config" && i + 1 < argc) {
      module_config = argv[++i];
    } else if (arg == "--default-module" && i + 1 < argc) {
      default_module_id = argv[++i];
    } else if (arg == "--module-id" && i + 1 < argc) {
      legacy_module_id = argv[++i];
    } else if (arg == "--node" && i + 1 < argc) {
      legacy_nodes.push_back(argv[++i]);
    } else if (arg == "--node-rpc-timeout-ms" && i + 1 < argc) {
      node_rpc_timeout_ms = std::atol(argv[++i]);
    }
  }

  std::vector<slotnexus::manager::ModuleDescriptor> modules;
  if (!module_config.empty()) {
    if (!LoadModuleConfig(module_config, &modules, &default_module_id)) {
      return 1;
    }
  } else {
    if (legacy_nodes.empty()) {
      legacy_nodes.push_back("tcp://127.0.0.1:19200");
    }
    slotnexus::manager::ModuleDescriptor descriptor;
    descriptor.module_id = legacy_module_id;
    descriptor.node_endpoints = legacy_nodes;
    modules.push_back(std::move(descriptor));
    if (default_module_id.empty()) {
      default_module_id = legacy_module_id;
    }
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  zmq::context_t ctx(1);
  try {
    slotnexus::manager::UnitManager manager(
        ctx, modules, default_module_id, /*max_tasks=*/0,
        std::chrono::milliseconds(node_rpc_timeout_ms));
    manager.bind(listen);
    std::cout << "unit_manager 监听 " << listen << "，模块 "
              << modules.size() << " 个（默认 " << manager.default_module_id()
              << "，RPC 超时 " << node_rpc_timeout_ms << " ms）" << std::endl;
    while (!g_stop) {
      manager.serve_once(std::chrono::milliseconds(100));
    }
    manager.close();
  } catch (const std::exception& e) {
    std::cerr << "unit_manager 启动失败: " << e.what() << std::endl;
    return 1;
  }
  std::cout << "unit_manager 已退出" << std::endl;
  return 0;
}
