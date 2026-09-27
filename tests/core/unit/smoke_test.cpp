// 冒烟测试：确认构建骨架可编译、链接并由 CTest 发现。
// Author: Caden
#include "slotnexus/version.hpp"

#include <cassert>
#include <iostream>

int main() {
  const std::string vs = slotnexus::version_string();
  assert(!vs.empty());
  assert(vs.find('.') != std::string::npos);
  assert(slotnexus::version().major >= 0);

  std::cout << "smoke_test ok, slotnexus version=" << vs << std::endl;
  return 0;
}
