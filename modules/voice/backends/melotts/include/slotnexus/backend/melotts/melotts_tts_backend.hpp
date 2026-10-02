// MeloTtsBackend：MeloTTS 离线中文/中英混读语音合成后端。
// Author: Caden
//
// 行为依据：板端已验证的 MeloTTS 推理路径。与 SummerTtsBackend 同契约：
//   - synthesize(text) 产出 kPcm…（每帧 ≤ kFrameSamples=320 采样，20 ms），
//     全部音频产出后 kDone；
//   - cancel() 后 synthesize 为空操作，不产出任何事件；
//   - set_event_callback 开启新会话：重置取消状态与统计。
//
// 本实现特性：
//   - 文本前端是词典 + token 表的自有实现（lexicon.txt / tokens.txt），不依赖
//     上游 tn/ 正则化与 Eigen；
//   - 编码器走 ONNX Runtime CPU（encoder-zh.onnx，8 输入 3 输出），解码器走
//     RKNN NPU（decoder-zh.rknn，2 输入 1 输出）；模型原生输出 44.1 kHz，
//     适配器内部线性重采样到契约 16 kHz；
//   - 说话人风格向量 g 由 g-zh_mix_en.bin 读取（恰好 256 个 float32）。
//
// 上游源码与模型均不随仓库分发（third_party/README.md 约定）；构建时由
// SLOTNEXUS_MELOTTS_ROOT 指向板端依赖根（含 ONNX Runtime 与 RKNN 的
// include/ + lib/），模型与文本资源路径经 config/taishanpi3m/session.json::tts
// 传入，不硬编码。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "slotnexus/backend/i_tts_backend.hpp"

namespace slotnexus::backend::melotts {

// 编码器与解码器的运行方式。当前固定分工为 ONNX Runtime CPU 编码器 +
// RKNN NPU 解码器；本后端只接受该组合，
// 避免把两个 ONNX 或两个 RKNN 误当成一条链路。
enum class MeloRuntime {
  kOnnxRuntimeCpu,
  kRknnNpu,
};

// MeloTTS 合成配置。路径均由调用方显式注入；数值上界在构造期校验，
// 非法配置直接抛 std::runtime_error（不带着坏参数进推理）。
struct MeloTtsConfig {
  // 编码器 ONNX 模型路径（encoder-zh.onnx）。
  std::string encoder_model_path;
  // 解码器 RKNN 模型路径（decoder-zh.rknn）。
  std::string decoder_model_path;
  // 文本前端资源：词典、音素 token 表、说话人风格 g 向量（1024 字节）。
  std::string lexicon_path;
  std::string tokens_path;
  std::string g_path;

  MeloRuntime encoder_runtime = MeloRuntime::kOnnxRuntimeCpu;
  MeloRuntime decoder_runtime = MeloRuntime::kRknnNpu;

  // 模型原生输出采样率（当前导出件 44100 Hz；适配器统一输出 16 kHz）。
  std::uint32_t native_sample_rate_hz = 44100;
  // 语速与 VITS 推理参数（板端已验证默认值）。
  float speed = 0.8f;
  float noise_scale = 0.3f;
  float noise_scale_w = 0.6f;
  float sdp_ratio = 0.2f;
  // ONNX Runtime intra-op 线程数；RKNN 侧同步执行，不创建线程。
  int intra_op_num_threads = 1;

  // RKNN NPU 核绑定：0=AUTO（默认），1=core0，2=core1，4=core2，
  // 3=core0+1，7=core0+1+2。多模型并发时可用它把 TTS 解码器钉在
  // RKLLM 未占用的 NPU 核上，避免解码器随机抢核拖慢生成。
  int decoder_core_mask = 0;

  // 单次 synthesize 的文本字节上限；超限显式失败，绝不静默截断。
  std::size_t max_text_bytes = 64u * 1024u;
  // 单个文本片段送入编码器的 interspersed phone 数上限（导出模型预算）。
  std::size_t max_encoder_phones = 240u;
  // 单个文本片段允许的 z_p 帧数上限。
  std::size_t max_z_frames_per_chunk = 8192u;
  // 单次 RKNN rknn_run 的有限等待毫秒数；0 表示不启用 SDK 超时。
  std::uint32_t run_timeout_ms = 30000u;
};

class MeloTtsBackend final : public ITtsBackend {
 public:
  // 加载 g 向量、文本前端、ONNX 编码器与 RKNN 解码器。
  // 任何资源不可用、配置非法或形状不符都抛出 std::runtime_error。
  explicit MeloTtsBackend(MeloTtsConfig config);
  ~MeloTtsBackend() override;

  MeloTtsBackend(const MeloTtsBackend&) = delete;
  MeloTtsBackend& operator=(const MeloTtsBackend&) = delete;

  void set_event_callback(EventCallback cb) override;
  void synthesize(const std::string& text) override;
  void cancel() override;

 private:
  struct Impl;  // ONNX Runtime / RKNN / 文本前端类型只存在于 .cpp
  std::unique_ptr<Impl> impl_;
};

}  // namespace slotnexus::backend::melotts
