// TSan 误报最小复现（Mock 冻结回归）：
// 教科书级正确的 mutex+cv 生产-消费模式，仅复用仓库的 BoundedQueue 头。
// 若本程序在 TSan 下也报 "double lock"/"data race"（两帧声称持有同一
// 把非递归锁），则证明报告是 TSan 运行时（gcc 11.4 + 内核 6.6）缺陷。
#include "voxorchestra/common/bounded_queue.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
  voxorchestra::common::BoundedQueue<int> q(8);
  std::atomic<bool> done{false};

  // 消费者：超时 pop 直到队列关闭。
  std::thread consumer([&] {
    int v = 0;
    while (true) {
      const auto r = q.pop_timeout(v, std::chrono::milliseconds(20));
      if (r == voxorchestra::common::QueueResult::kClosed) {
        break;
      }
    }
    done.store(true);
  });

  // 生产者：推入一些数据后关闭队列（与 SessionPipeline 收尾相同模式）。
  for (int i = 0; i < 100; ++i) {
    q.push(i);
  }
  q.close();
  consumer.join();
  if (!done.load()) {
    return 1;
  }
  std::cout << "repro ok" << std::endl;
  return 0;
}
