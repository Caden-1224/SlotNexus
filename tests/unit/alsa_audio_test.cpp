// AlsaAudioSink / AlsaAudioSource 单元测试（板端真实声卡；仅硬件后端
// 构建时编译，见 tests/unit/CMakeLists.txt）。
//
// 设备名经环境变量 VOXORCHESTRA_ALSA_DEVICE 注入（tests/unit/CMakeLists.txt
// 由 VOXORCHESTRA_ALSA_DEVICE 缓存变量设置；未配置时测试用 "default"）；
// 无模型 env 依赖，不跳过条件只依赖硬件可用——open 失败记录并跳过
// （板端无声卡时不能挂死）。
//
//   sink：open→write→close 生命周期 / 未 close 重复 open → false /
//         未 open write_pcm → false / drain 语义（写入后 close 返回 true，
//         即 snd_pcm_drain 全量播完不失败）/ close 幂等 / close 后可重开；
//   source：open 后读 N 帧长度正确 / 数据非全零（RMS > 阈值，板端环境音
//         或说话声即可）/ close 幂等 / 未 open read 返回空帧。
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "voxorchestra/backend/alsa/alsa_audio_sink.hpp"
#include "voxorchestra/backend/alsa/alsa_audio_source.hpp"

namespace eas = voxorchestra::backend::alsa;

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

std::string device_from_env() {
  const char* p = std::getenv("VOXORCHESTRA_ALSA_DEVICE");
  return (p != nullptr && *p != '\0') ? p : "default";
}

// 确定性 440 Hz 方波（1 s，S16 满幅半幅），作为播放内容（可听 + 可断言）。
std::vector<int16_t> make_tone(int samples) {
  std::vector<int16_t> out(static_cast<std::size_t>(samples));
  const int period = 16000 / 440;  // 440 Hz @16 kHz
  for (int i = 0; i < samples; ++i) {
    out[static_cast<std::size_t>(i)] =
        (i % period < period / 2) ? 12000 : -12000;
  }
  return out;
}

// 生命周期：open→write→close 全绿；重复 open（未 close）false；
// 未 open write_pcm false；close 幂等；close 后可重开。
void test_sink_lifecycle(const std::string& device) {
  const int kSamples = 16000;  // 1 s
  eas::AlsaAudioSink sink(device, 16000);
  CHECK(!sink.write_pcm(make_tone(kSamples)));  // 未 open → false
  if (!sink.open()) {
    std::cout << "  [skip] sink.open 失败（设备 " << device
              << " 不可用，声卡未就绪?）" << std::endl;
    return;
  }
  CHECK(!sink.open());  // 未 close 重复 open → false
  const int actual = sink.actual_sample_rate();
  CHECK(actual > 0);
  CHECK(sink.write_pcm(make_tone(kSamples)));
  CHECK(sink.write_pcm(make_tone(kSamples)));
  CHECK(sink.close());       // drain（等全部播完）+ close
  CHECK(sink.close());       // 幂等
  CHECK(sink.open());        // close 后可重开（新生命周期）
  CHECK(sink.close());
  std::printf("  [ok] sink 生命周期（实际采样率 %d Hz，1 s×2 播放）\n", actual);
}

// 非原生率回退：22050 是 ES8323（plughw:0,0）不支持的输入率（default
// 经 plug 层直通），用于覆盖 open() 的候选率阶梯回退 + 线性重采样路径
// （内容率 16000 在两设备均原生支持、不触发回退，故另用 22050 锻炼该
// 路径）。实际率打印供证据记录（不硬编码——硬件约束见板端实测）。
void test_sink_rate_fallback(const std::string& device) {
  eas::AlsaAudioSink sink(device, 22050);
  if (!sink.open()) {
    std::cout << "  [skip] sink.open(22050) 失败（设备 " << device
              << " 不可用?）" << std::endl;
    return;
  }
  const int actual = sink.actual_sample_rate();
  CHECK(actual > 0);
  CHECK(sink.write_pcm(make_tone(22050)));  // 1 s @22050（回退时内部重采样）
  CHECK(sink.close());
  std::printf("  [ok] sink 22050 Hz 请求成功（实际 %d Hz）\n", actual);
}

// drain 语义独立验证：写入短数据后 close 立即返回 true（不阻塞至超时）。
void test_sink_drain_short(const std::string& device) {
  eas::AlsaAudioSink sink(device, 16000);
  if (!sink.open()) {
    return;  // 上一用例已跳过说明
  }
  CHECK(sink.write_pcm(make_tone(1600)));  // 100 ms
  CHECK(sink.close());
  std::cout << "  [ok] sink drain：100 ms 数据 close 返回 true" << std::endl;
}

// 录音：open 后读 N 帧长度正确、数据非全零（RMS > 阈值）、close 幂等。
void test_source_read(const std::string& device) {
  eas::AlsaAudioSource source(device, 16000);
  CHECK(source.read(1600).empty());  // 未 open → 空帧
  if (!source.open()) {
    std::cout << "  [skip] source.open 失败（设备 " << device
              << " 不可用，声卡未就绪?）" << std::endl;
    return;
  }
  CHECK(!source.open());  // 未 close 重复 open → false
  const int actual = source.actual_sample_rate();
  CHECK(actual > 0);
  std::vector<int16_t> all;
  // 读 5 段 × 1600 采样（0.5 s @16 kHz），段间不丢弃。
  for (int i = 0; i < 5; ++i) {
    const auto frame = source.read(1600);
    CHECK(frame.size() == 1600);  // 阻塞模式满读（overrun 时为空帧属例外）
    all.insert(all.end(), frame.begin(), frame.end());
  }
  double sum_sq = 0.0;
  for (int16_t v : all) {
    sum_sq += static_cast<double>(v) * static_cast<double>(v);
  }
  const double rms = std::sqrt(sum_sq / static_cast<double>(all.size()));
  std::printf("  [info] 录音 RMS = %.1f（%zu 采样，实际 %d Hz）\n", rms,
              all.size(), actual);
  CHECK(rms > 1.0);  // 非全零：环境音/说话声足够；数字静音则为 0
  CHECK(source.close());
  CHECK(source.close());  // 幂等
  std::cout << "  [ok] source 录音：帧长正确，RMS 非零，close 幂等" << std::endl;
}

}  // namespace

int main() {
  std::cout << "alsa_audio_test:" << std::endl;
  const std::string device = device_from_env();
  std::cout << "  设备: " << device << std::endl;
  try {
    test_sink_lifecycle(device);
    test_sink_rate_fallback(device);
    test_sink_drain_short(device);
    test_source_read(device);
  } catch (const std::exception& e) {
    std::cerr << "  [fail] 异常: " << e.what() << std::endl;
    ++g_failures;
  }

  if (g_failures == 0) {
    std::cout << "alsa_audio_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "alsa_audio_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
