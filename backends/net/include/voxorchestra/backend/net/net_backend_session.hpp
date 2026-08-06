// 数据面网络后端共享驱动：把远端节点（asr/llm/tts）当作本地流式后端。
//
// 上行（控制面）：RPC 直连节点 REP 端点——setup 建立任务、inference 驱动
//   一次推理、cancel 中止（REQ/REP，全部带 deadline，无硬等待）；
// 下行（数据面）：节点 --events PUB 端点订阅回放——partial/token/PCM/done
//   事件实时经回调投递（与本地 Fake 后端同一 EventCallback 契约）。
//
// 一次推理（drive_inference）：
//   1. 订阅本轮事件流主题 <work_id>/<request_id>/，等待订阅传播（slow
//      joiner：发布端在推理开始时才 publish，订阅先于发布的契约）；
//   2. call_async 发出 inference 请求（不阻塞），进入轮询循环；
//   3. 每轮先非阻塞收完已到达事件（按主题精确过滤，丢弃旧流残留）并
//      实时回放，再 poll_response 短等 RPC 响应；
//   4. 循环直到：响应到达（完成，返回 ack 文本）/ 取消（提前返回空）/
//      超时（抛异常，调用方决定会话失败语义）。
//
// 节点侧 REP 串行处理：inference 请求阻塞期间 cancel 请求排队，无法中途
// 打断节点推理（已知限制，见 apps/common/runtime_node.cpp）。因此取消以
// 会话侧立即退出为语义，节点推理跑完但结果被丢弃（fake 链路瞬时，无害）。
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>
#include <zmq.hpp>

#include "voxorchestra/backend/backend_event.hpp"
#include "voxorchestra/dataplane/event_channel.hpp"
#include "voxorchestra/transport/rpc.hpp"

namespace voxorchestra::backend::net {

// 节点连接配置（会话侧注入，来自 session_node 的 net 模式参数）。
struct NetBackendConfig {
  std::string rpc_endpoint;       // 节点 RPC（REP）端点
  std::string events_endpoint;    // 节点数据面事件 PUB 端点（--events）
  std::string events_sync;        // 订阅握手端点（--events-sync）
  std::string work_id;            // 节点侧任务 id（与会话 work_id 一致）
  std::chrono::milliseconds setup_timeout{5000};     // setup RPC 等待
  std::chrono::milliseconds rpc_timeout{30000};      // 单次推理等待上限
  std::chrono::milliseconds subscribe_settle{100};   // 订阅传播等待（回环余量）
  // ASR 音频上行（真实负载模式）：会话侧把累积 PCM 上行到 asr 节点，
  // 由节点真实后端（sherpa_onnx）识别；false 时为 Mock 帧数约定
  // （{"text": "<帧数>"}，与 fake 节点一致）。仅 NetAsrBackend 使用。
  bool asr_audio_uplink = false;
};

// 一次推理的驱动会话（三个网络后端共享；非线程安全，仅驱动线程使用，
// cancel() 除外——cancel 可被控制面线程并发调用）。
class NetBackendSession {
 public:
  explicit NetBackendSession(zmq::context_t& ctx, NetBackendConfig config);
  ~NetBackendSession();

  NetBackendSession(const NetBackendSession&) = delete;
  NetBackendSession& operator=(const NetBackendSession&) = delete;

  // 设置事件回调（每次推理会话开始前调用，语义与本地后端一致）。
  void set_event_callback(EventCallback cb);

  // 节点 setup（建立任务）。失败抛 std::runtime_error（节点不可达/超时）。
  void setup();

  // 协作式取消：置位本地标志（轮询循环据此提前返回）并向节点发 cancel
  // （fire-and-forget，晚到无 ACK 是预期行为；节点不可达时静默忽略）。
  // 可被任意线程调用；多次调用幂等。
  void cancel();

  // 驱动一次推理。request_id 标识本轮事件流（调用方保证唯一）；
  // payload 为节点负载约定（如 {"text": ...}）。成功返回 ack 文本；
  // 取消返回空；超时或节点错误抛 std::runtime_error。
  std::string drive_inference(const std::string& request_id,
                              const nlohmann::json& payload);

  // 子请求 id：按阶段独立递增（ASR 的 "a0"/"a1"、LLM 的 "l0"、TTS 的
  // "t0"/"t1"...），保证事件流主题唯一。供 IAsrBackend 等实现调用。
  std::string next_request_id(const std::string& stage);

  // 构造注入的配置（只读，供后端实现查询负载模式等）。
  const NetBackendConfig& config() const { return config_; }

 private:
  zmq::context_t& ctx_;
  NetBackendConfig config_;
  transport::RpcClient rpc_;        // 推理 RPC（驱动线程独占）
  transport::RpcClient cancel_rpc_; // cancel RPC（仅 cancel() 使用）
  dataplane::EventSubscriber sub_;  // 事件订阅（订阅累积，按主题过滤）
  EventCallback cb_;
  std::atomic<bool> cancelled_{false};
  std::atomic<std::uint64_t> seq_{0};
  // 上一轮推理请求已发出但未确认完成（取消/超时/异常退出）：REQ 停在
  // 等待响应阶段，下一轮 drive 先重建 socket（驱动线程独占）。
  bool rpc_pending_ = false;
};

}  // namespace voxorchestra::backend::net
