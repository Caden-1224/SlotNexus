// SlotNexus 版本信息。
// Author: Caden
// 注意：与根 CMakeLists.txt 的 project(slotnexus VERSION ...) 保持一致。
#pragma once

#include <string>

namespace slotnexus {

struct Version {
  int major = 0;
  int minor = 0;
  int patch = 0;
};

// 当前编译进库的版本号。
const Version& version();

// 形如 "0.2.0" 的版本字符串。
const std::string& version_string();

}  // namespace slotnexus
