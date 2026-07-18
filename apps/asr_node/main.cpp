// asr_node 可执行入口：Fake 语音识别节点。
//
// 用法：asr_node [--listen tcp://127.0.0.1:19201]
// 默认端口约定：echo 19200 / asr 19201 / rag 19202 / llm 19203 / tts 19204。
//
// Node 外壳（RuntimeNode + TaskRuntime）只依赖接口；本文件实现 IBackend
// 适配器，把流式 FakeAsrBackend 驱动到完成并返回最终文本：
//   - Mock 负载约定：客户端发 {"text": "<帧数N>"}；RuntimeNode 已提取
//     text 字段，适配器收到纯文本 "<N>"，用 FakeAudioSource 合成 N 帧
//     确定性 PCM 送入 FakeAsrBackend；
//   - 返回最终识别文本（kFinal 事件的文本）。
// Fake 用于测试协议与编排，不是真实模型；未来 SherpaAsrBackend 只换工厂。
// SIGINT/SIGTERM 优雅退出（退出码 0）。
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <zmq.hpp>

#include "voxorchestra/backend/backend_event.hpp"
#include "voxorchestra/backend/fake/fake_asr_backend.hpp"
#include "voxorchestra/backend/fake/fake_audio_source.hpp"
#include "voxorchestra/runtime/ibackend.hpp"
#include "runtime_node.hpp"

namespace {

// IBackend 适配器：把 FakeAsrBackend 的流式事件驱动到完成。
// 每帧间协作式检查 cancelled / deadline，命中即取消后端并尽快返回。
class AsrNodeBackend final : public voxorchestra::runtime::IBackend {
 public:
  voxorchestra::runtime::BackendResult infer(
      const std::string& payload,
      std::chrono::steady_clock::time_point deadline,
      const std::atomic<bool>& cancelled) override {
    // Mock 约定：payload 为提取后的纯文本 "<帧数>"；非法或非正数按 1 帧处理。
    int frames = 1;
    try {
      frames = std::stoi(payload);
      if (frames <= 0) {
        frames = 1;
      }
    } catch (...) {
      frames = 1;
    }

    std::string final_text;
    asr_.set_event_callback([&final_text](const voxorchestra::backend::BackendEvent& e) {
      if (e.kind == voxorchestra::backend::BackendEvent::Kind::kFinal) {
        final_text = e.text;
      }
    });

    for (int i = 0; i < frames; ++i) {
      if (cancelled.load()) {
        asr_.cancel();
        return {voxorchestra::runtime::BackendResult::Code::kCancelled, {}};
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        asr_.cancel();
        return {voxorchestra::runtime::BackendResult::Code::kTimeout, {}};
      }
      asr_.feed_audio(voxorchestra::backend::fake::FakeAudioSource::make_frame(
                          static_cast<std::uint32_t>(i)),
                      i + 1 == frames);
    }
    return {voxorchestra::runtime::BackendResult::Code::kOk, std::move(final_text)};
  }

 private:
  voxorchestra::backend::fake::FakeAsrBackend asr_;
};

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int /*sig*/) { g_stop = 1; }

}  // namespace

int main(int argc, char** argv) {
  std::string listen = "tcp://127.0.0.1:19201";
  for (int i = 1; i < argc - 1; ++i) {
    if (std::string(argv[i]) == "--listen") {
      listen = argv[i + 1];
    }
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  zmq::context_t ctx(1);
  auto runtime = std::make_unique<voxorchestra::runtime::TaskRuntime>(
      [] { return std::make_shared<AsrNodeBackend>(); });
  voxorchestra::node::RuntimeNode node(ctx, std::move(runtime));
  try {
    node.bind(listen);
    std::cout << "asr_node 监听 " << listen << "（FakeAsr 后端）" << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "asr_node 启动失败: " << e.what() << std::endl;
    return 1;
  }

  while (!g_stop) {
    node.serve_once(std::chrono::milliseconds(100));
  }
  node.close();
  std::cout << "asr_node 已退出" << std::endl;
  return 0;
}
