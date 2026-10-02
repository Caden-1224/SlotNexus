// MeloTTS Decoder 切片不变量回归（纯算法，硬件后端构建下运行）。
// Author: Caden
//
// 保护 build_decode_slices 的行为：切片按帧连续覆盖 [0,total)，切片数等于
// ceil(total/frames_per_call)，每片 1..W 帧；空输入或预算为 0 明确失败。
#include <cstddef>
#include <cstdio>
#include <vector>

#include "melotts_internal.hpp"

namespace melo = slotnexus::backend::melotts;

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

std::size_t total_frames(const std::vector<std::size_t>& values) {
  std::size_t total = 0;
  for (const std::size_t value : values) {
    total += value;
  }
  return total;
}

void check(const std::vector<std::size_t>& units, std::size_t frames_per_call) {
  const std::size_t total = total_frames(units);
  const auto result = melo::build_decode_slices(units, frames_per_call);
  if (total == 0U) {
    expect(!result.ok(), "空输入应失败");
    return;
  }
  expect(result.ok(), "正常输入应成功");
  if (!result.ok()) {
    return;
  }
  const auto& slices = result.value;
  expect(slices.size() == (total + frames_per_call - 1U) / frames_per_call,
         "切片数应为 ceil(total/W)");
  std::size_t cursor = 0U;
  for (const auto& slice : slices) {
    expect(slice.frame_begin == cursor, "切片必须连续覆盖");
    expect(slice.frame_end > slice.frame_begin, "切片必须非空");
    expect(slice.frame_end - slice.frame_begin <= frames_per_call,
           "切片不得超过解码窗");
    cursor = slice.frame_end;
  }
  expect(cursor == total, "最后一个切片必须覆盖到 total");
}

}  // namespace

int main() {
  check({}, 128U);
  check({0U}, 128U);
  check({72U}, 128U);
  check({128U}, 128U);
  check({353U}, 128U);
  check({100U, 100U, 100U}, 128U);
  check({512U}, 256U);
  check({1000U, 500U}, 128U);
  check({1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U}, 16U);

  const auto zero_budget = melo::build_decode_slices({10U}, 0U);
  expect(!zero_budget.ok(), "帧数预算为 0 应失败");

  if (failures == 0) {
    std::printf("melotts_decode_slices_test 通过\n");
    return 0;
  }
  std::fprintf(stderr, "melotts_decode_slices_test 失败 %d 项\n", failures);
  return 1;
}
