// BoundedQueue 单元测试：容量、满队列行为（拒绝/超时/阻塞）、关闭唤醒、
// 清空、峰值统计与多线程并发。
#include "voxorchestra/common/bounded_queue.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace cq = voxorchestra::common;
using cq::QueueResult;

using namespace std::chrono_literals;

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

// 基本容量与拒绝语义（满队列行为之一：拒绝）。
void test_capacity_and_reject() {
  cq::BoundedQueue<std::string> q(2);
  CHECK(q.capacity() == 2);
  CHECK(q.try_push("a") == QueueResult::kOk);
  CHECK(q.try_push("b") == QueueResult::kOk);
  CHECK(q.try_push("c") == QueueResult::kFull);  // 已满：拒绝
  CHECK(q.size() == 2);

  std::string out;
  CHECK(q.try_pop(out) == QueueResult::kOk && out == "a");
  CHECK(q.try_push("c") == QueueResult::kOk);  // 消费后可再入队
  CHECK(q.size() == 2);
  // 空队列：拒绝语义的另一侧。
  cq::BoundedQueue<int> empty_q(1);
  int v = 0;
  CHECK(empty_q.try_pop(v) == QueueResult::kEmpty);
  std::cout << "  [ok] 容量与拒绝：满返回 kFull、空返回 kEmpty" << std::endl;
}

// 超时语义：满队列 push 等待超时后返回 kFull（超时行为明确）。
void test_push_timeout_full() {
  cq::BoundedQueue<int> q(1);
  CHECK(q.push_timeout(1, 0ms) == QueueResult::kOk);
  const auto t0 = std::chrono::steady_clock::now();
  CHECK(q.push_timeout(2, 50ms) == QueueResult::kFull);
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  CHECK(elapsed >= 40ms);  // 确实等待了超时时间
  std::cout << "  [ok] push 超时：满队列等待 timeout 后返回 kFull" << std::endl;
}

// 阻塞推入：满时阻塞，消费者取出后解除阻塞（背压让路）。
void test_blocking_push_wakes_on_pop() {
  cq::BoundedQueue<int> q(1);
  CHECK(q.push(1));
  std::atomic<bool> done{false};
  std::thread producer([&] {
    CHECK(q.push(2));  // 阻塞直到有空间
    done.store(true);
  });
  // 生产者应处于阻塞；消费一个后解除。
  std::this_thread::sleep_for(50ms);
  CHECK(!done.load());
  int v = 0;
  CHECK(q.try_pop(v) == QueueResult::kOk && v == 1);
  producer.join();
  CHECK(done.load());
  std::cout << "  [ok] 阻塞推入：满时挂起、消费后继续（背压）" << std::endl;
}

// 空队列 pop 超时返回 kEmpty；有数据后解除阻塞。
void test_pop_timeout_and_wake() {
  cq::BoundedQueue<int> q(2);
  int v = 0;
  CHECK(q.pop_timeout(v, 30ms) == QueueResult::kEmpty);
  std::thread producer([&] {
    std::this_thread::sleep_for(40ms);
    CHECK(q.push(42));
  });
  CHECK(q.pop_timeout(v, 2000ms) == QueueResult::kOk && v == 42);
  producer.join();
  std::cout << "  [ok] pop 超时与唤醒：空时等待、数据到达后立即返回" << std::endl;
}

// 关闭语义：唤醒全部等待者；重复 close 幂等；关闭后已有数据仍可 pop。
void test_close_semantics() {
  cq::BoundedQueue<int> q(2);
  CHECK(q.push(1));
  q.close();
  q.close();  // 幂等

  int v = 0;
  CHECK(q.try_pop(v) == QueueResult::kOk && v == 1);  // 已有关闭前数据可取出
  CHECK(q.try_pop(v) == QueueResult::kClosed);        // 之后为空 + 关闭
  CHECK(q.try_push(2) == QueueResult::kClosed);
  CHECK(q.push_timeout(2, 10ms) == QueueResult::kClosed);

  // 关闭唤醒阻塞中的消费者与生产者。
  cq::BoundedQueue<int> wq(1);
  std::atomic<int> pop_waiter{0};
  std::thread waiter([&] {
    const bool ok = wq.pop(v);  // 阻塞，等待 close 唤醒
    pop_waiter.store(ok ? 1 : 2);
  });
  std::this_thread::sleep_for(30ms);
  wq.close();
  waiter.join();
  CHECK(pop_waiter.load() == 2);  // pop 返回 false（关闭）
  std::cout << "  [ok] 关闭语义：幂等、唤醒等待者、关闭前数据可取出"
            << std::endl;
}

// 清空与峰值统计：clear 清空并唤醒生产者；peak 记录历史最大深度。
void test_clear_and_peak() {
  cq::BoundedQueue<int> q(3);
  CHECK(q.push(1));
  CHECK(q.push(2));
  CHECK(q.peak() == 2);
  CHECK(q.clear() == 2);
  CHECK(q.empty() && q.size() == 0);
  CHECK(q.peak() == 2);  // 峰值保留历史最大值

  // 峰值不超过容量：压力下反复填满。
  cq::BoundedQueue<int> pq(4);
  for (int i = 0; i < 100; ++i) {
    while (pq.try_push(i) == QueueResult::kOk) {
    }
    CHECK(pq.size() <= 4);
    int x = 0;
    while (pq.try_pop(x) == QueueResult::kOk) {
    }
  }
  CHECK(pq.peak() <= 4);
  std::cout << "  [ok] 清空与峰值：clear 返回条数、peak 不超过容量" << std::endl;
}

// 并发压力：多生产者多消费者，总数守恒、无数据损坏。
void test_concurrent_producers_consumers() {
  constexpr int kProducers = 2;
  constexpr int kConsumers = 2;
  constexpr int kPerProducer = 500;
  cq::BoundedQueue<int> q(8);
  std::atomic<int> sum{0};

  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p] {
      for (int i = 0; i < kPerProducer; ++i) {
        // 满时稍等重试（不丢失数据）。
        while (q.push_timeout(p * 1000 + i, 5ms) != QueueResult::kOk) {
        }
      }
    });
  }
  std::vector<std::thread> consumers;
  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&] {
      int v = 0;
      for (;;) {
        const auto r = q.pop_timeout(v, 5ms);
        if (r == QueueResult::kClosed) {
          break;  // 生产结束且已排空
        }
        if (r == QueueResult::kOk) {
          sum.fetch_add(v);
        }
        // kEmpty：短暂空窗，继续等待。
      }
    });
  }
  for (auto& t : producers) {
    t.join();
  }
  // 生产者结束后关闭，消费者排空并退出。
  q.close();
  for (auto& t : consumers) {
    t.join();
  }
  // 期望总和 = 每个生产者 0..(kPerProducer-1) 加上编号偏移。
  long expected = 0;
  for (int p = 0; p < kProducers; ++p) {
    for (int i = 0; i < kPerProducer; ++i) {
      expected += p * 1000 + i;
    }
  }
  CHECK(sum.load() == expected);
  CHECK(q.empty());
  std::cout << "  [ok] 并发压力：2 生产者 × 2 消费者，数据守恒无丢失"
            << std::endl;
}

}  // namespace

int main() {
  std::cout << "bounded_queue_test:" << std::endl;
  test_capacity_and_reject();
  test_push_timeout_full();
  test_blocking_push_wakes_on_pop();
  test_pop_timeout_and_wake();
  test_close_semantics();
  test_clear_and_peak();
  test_concurrent_producers_consumers();

  if (g_failures == 0) {
    std::cout << "bounded_queue_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "bounded_queue_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
