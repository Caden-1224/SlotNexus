// VoxOrchestra 版本信息。
// 注意：与根 CMakeLists.txt 的 project(voxorchestra VERSION ...) 保持一致。
#pragma once

#include <string>

namespace voxorchestra {

struct Version {
  int major = 0;
  int minor = 0;
  int patch = 0;
};

// 当前编译进库的版本号。
const Version& version();

// 形如 "0.1.0" 的版本字符串。
const std::string& version_string();

}  // namespace voxorchestra
