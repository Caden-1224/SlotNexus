// 测试路径助手：默认测试资产根与仓库根解析。
//
// Author: Caden
// 设计意图：测试可执行文件不假设固定向上三级路径，也不硬编码 Windows/
// WSL 绝对路径。CTest 通过 VOXORCHESTRA_TEST_ROOT 注入构建期生成的最小
// 测试根（含 data/knowledge、data/fixtures 与 config/mock）。
#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

namespace vox_test {

inline std::string EnvOrEmpty(const char* name) {
  const char* value = std::getenv(name);
  return (value == nullptr) ? std::string() : std::string(value);
}

inline std::string RootFromEnv() { return EnvOrEmpty("VOXORCHESTRA_TEST_ROOT"); }

// 默认测试运行根。优先环境变量；否则向上查找构建目录旁的 test-root。
inline std::string Root(const char* argv0) {
  const std::string from_env = EnvOrEmpty("VOXORCHESTRA_TEST_ROOT");
  if (!from_env.empty()) {
    return from_env;
  }
  std::filesystem::path p =
      std::filesystem::absolute(std::filesystem::path(argv0).parent_path());
  // 直接运行构建树二进制时，向上找到含 CMakeCache.txt 的构建根。
  std::filesystem::path build_root = p;
  for (int i = 0; i < 8 && !build_root.empty(); ++i) {
    if (std::filesystem::exists(build_root / "CMakeCache.txt") &&
        std::filesystem::exists(build_root / "test-root")) {
      return (build_root / "test-root").string();
    }
    build_root = build_root.parent_path();
  }
  // 兜底：旧式“向上三级”相对仓库根；调用方应优先依赖 CTest 环境变量。
  std::filesystem::path fallback = p;
  for (int i = 0; i < 3 && !fallback.empty(); ++i) {
    fallback = fallback.parent_path();
  }
  return fallback.string();
}

}  // namespace vox_test
