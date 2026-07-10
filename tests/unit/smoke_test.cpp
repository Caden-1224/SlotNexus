// 冒烟测试：确认构建骨架可编译、链接并由 CTest 发现。
#include "voxorchestra/version.hpp"

#include <cassert>
#include <iostream>

int main() {
  const std::string vs = voxorchestra::version_string();
  assert(!vs.empty());
  assert(vs.find('.') != std::string::npos);
  assert(voxorchestra::version().major >= 0);

  std::cout << "smoke_test ok, voxorchestra version=" << vs << std::endl;
  return 0;
}
