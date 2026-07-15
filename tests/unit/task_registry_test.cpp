// TaskRegistry 单元测试：分配唯一性、查询、幂等释放、容量上限、并发安全。
#include "voxorchestra/task_registry/task_registry.hpp"

#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace etr = voxorchestra::task_registry;

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

void test_allocate_unique_increasing() {
  etr::TaskRegistry registry;
  const std::string a = registry.allocate();
  const std::string b = registry.allocate();
  const std::string c = registry.allocate();

  CHECK(a == "w-0");
  CHECK(b == "w-1");
  CHECK(c == "w-2");
  CHECK(a != b && b != c && a != c);
  CHECK(registry.size() == 3);
  std::cout << "  [ok] work_id 单调递增且互不相同" << std::endl;
}

void test_find() {
  etr::TaskRegistry registry;
  const std::string id = registry.allocate();
  CHECK(registry.find(id));
  CHECK(!registry.find("w-999"));
  CHECK(!registry.find(""));
  std::cout << "  [ok] find 对存活任务返回 true，未知 id 返回 false" << std::endl;
}

void test_release_idempotent() {
  etr::TaskRegistry registry;
  const std::string id = registry.allocate();

  CHECK(registry.release(id));
  CHECK(!registry.find(id));
  CHECK(registry.size() == 0);
  // 幂等：重复释放返回 false，不抛异常。
  CHECK(!registry.release(id));
  CHECK(!registry.release("w-12345"));
  std::cout << "  [ok] 释放幂等：重复释放与未知 id 均返回 false" << std::endl;
}

void test_no_id_reuse_after_release() {
  etr::TaskRegistry registry;
  const std::string a = registry.allocate();  // w-0
  registry.release(a);

  // 释放后重新分配得到新 id，不复用旧 id（避免旧请求串到新任务）。
  const std::string b = registry.allocate();
  CHECK(b == "w-1");
  CHECK(!registry.find("w-0"));
  std::cout << "  [ok] 释放后不回收复用 id" << std::endl;
}

void test_capacity_limit() {
  etr::TaskRegistry registry(3);
  CHECK(!registry.allocate().empty());
  CHECK(!registry.allocate().empty());
  CHECK(!registry.allocate().empty());
  CHECK(registry.allocate().empty());  // 容量耗尽
  CHECK(registry.size() == 3);

  registry.release("w-0");
  CHECK(!registry.allocate().empty());  // 释放后恢复一个名额
  std::cout << "  [ok] 容量上限生效，释放后名额恢复" << std::endl;
}

void test_concurrent_allocate_unique() {
  etr::TaskRegistry registry(1000);  // 容量要能容纳全部并发分配
  constexpr int kThreads = 8;
  constexpr int kPerThread = 100;

  std::vector<std::thread> threads;
  std::vector<std::vector<std::string>> per_thread(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&registry, &per_thread, i] {
      for (int j = 0; j < kPerThread; ++j) {
        const std::string id = registry.allocate();
        CHECK(!id.empty());
        per_thread[i].push_back(id);
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }

  std::set<std::string> all;
  std::size_t total = 0;
  for (const auto& list : per_thread) {
    total += list.size();
    for (const auto& id : list) {
      all.insert(id);
    }
  }
  CHECK(all.size() == total);  // 无重复
  CHECK(registry.size() == total);
  std::cout << "  [ok] 8 线程并发分配 " << total << " 个 id 全部唯一" << std::endl;
}

}  // namespace

int main() {
  std::cout << "task_registry_test:" << std::endl;
  test_allocate_unique_increasing();
  test_find();
  test_release_idempotent();
  test_no_id_reuse_after_release();
  test_capacity_limit();
  test_concurrent_allocate_unique();

  if (g_failures == 0) {
    std::cout << "task_registry_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "task_registry_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
