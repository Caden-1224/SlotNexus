// Backend 契约单元测试：统一事件 BackendEvent 与音频常量。
//
// 五类接口（IAsrBackend / IRetriever / ILlmBackend / ITtsBackend /
// IAudioSink）为纯虚契约，无实现逻辑可测；本测试锁定事件结构与常量，
// 防止契约演进时破坏既有语义。
#include "voxorchestra/backend/backend_event.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace eb = voxorchestra::backend;

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

// BackendEvent 默认构造：kDone、文本与 PCM 为空。
void test_event_default() {
  eb::BackendEvent e;
  CHECK(e.kind == eb::BackendEvent::Kind::kDone);
  CHECK(e.text.empty());
  CHECK(e.pcm.empty());
  std::cout << "  [ok] BackendEvent 默认构造：kDone / 空文本 / 空 PCM" << std::endl;
}

// 事件可携带文本与 PCM，Kind 字符串化覆盖全部五类。
void test_event_fields_and_kind_names() {
  eb::BackendEvent partial;
  partial.kind = eb::BackendEvent::Kind::kPartial;
  partial.text = "第1帧";
  CHECK(partial.text == "第1帧");
  CHECK(std::string(eb::to_string(partial.kind)) == "partial");

  eb::BackendEvent pcm;
  pcm.kind = eb::BackendEvent::Kind::kPcm;
  pcm.pcm = {std::int16_t(1), std::int16_t(-2), std::int16_t(3)};
  CHECK(pcm.pcm.size() == 3);
  CHECK(pcm.pcm[0] == 1 && pcm.pcm[1] == -2 && pcm.pcm[2] == 3);
  CHECK(std::string(eb::to_string(pcm.kind)) == "pcm");

  CHECK(std::string(eb::to_string(eb::BackendEvent::Kind::kFinal)) == "final");
  CHECK(std::string(eb::to_string(eb::BackendEvent::Kind::kToken)) == "token");
  CHECK(std::string(eb::to_string(eb::BackendEvent::Kind::kDone)) == "done");
  std::cout << "  [ok] 事件字段与 Kind 字符串化：partial/final/token/pcm/done" << std::endl;
}

// 音频常量：16 kHz 单声道，20 ms 帧 = 320 采样。
void test_audio_constants() {
  CHECK(eb::kSampleRateHz == 16000);
  CHECK(eb::kChannels == 1);
  CHECK(eb::kFrameSamples == 320);
  CHECK(eb::kFrameSamples == eb::kSampleRateHz / 50);  // 20 ms @ 16 kHz
  std::cout << "  [ok] 音频常量：16 kHz 单声道、20 ms 帧（320 采样）" << std::endl;
}

// 契约头文件可编译引用（纯头文件，无第三方依赖）。
void test_contract_headers_compile() {
  eb::EventCallback cb = [](const eb::BackendEvent&) {};
  eb::BackendEvent e;
  cb(e);
  std::cout << "  [ok] EventCallback 与事件头文件可独立编译" << std::endl;
}

}  // namespace

int main() {
  std::cout << "backend_contract_test:" << std::endl;
  test_event_default();
  test_event_fields_and_kind_names();
  test_audio_constants();
  test_contract_headers_compile();

  if (g_failures == 0) {
    std::cout << "backend_contract_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "backend_contract_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
