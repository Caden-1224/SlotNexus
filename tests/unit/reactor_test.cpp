// EventLoop/Channel/Poller 单元测试：
// 跨线程任务投递、pipe 读写事件、对端关闭错误事件、quit 语义、线程内直接执行。
#include "voxorchestra/network/channel.hpp"
#include "voxorchestra/network/event_loop.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace en = voxorchestra::network;
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

// 带截止时间的条件等待，避免测试挂死。
bool wait_until(const std::function<bool()>& cond, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (cond()) {
      return true;
    }
    std::this_thread::sleep_for(2ms);
  }
  return cond();
}

void test_task_from_other_thread() {
  en::EventLoop loop;
  std::atomic<int> executed{0};
  std::thread::id loop_tid;

  std::thread t([&loop] { loop.run(); });

  for (int i = 0; i < 3; ++i) {
    loop.run_in_loop([&] {
      loop_tid = std::this_thread::get_id();
      executed.fetch_add(1);
    });
  }

  CHECK(wait_until([&] { return executed.load() == 3; }, 1000ms));
  CHECK(loop_tid == t.get_id());  // 任务确实在事件循环线程执行

  loop.quit();
  t.join();
  std::cout << "  [ok] 跨线程投递 3 个任务，均在事件循环线程执行" << std::endl;
}

void test_pipe_read_event() {
  int fds[2];
  CHECK(::pipe(fds) == 0);
  const int read_fd = fds[0];
  const int write_fd = fds[1];

  en::EventLoop loop;
  std::string received;
  std::atomic<bool> got_read{false};
  en::Channel* ch = nullptr;  // 生命周期由 loop 线程管理

  std::thread t([&loop] { loop.run(); });

  // Channel 的创建与注册必须在 loop 线程完成。
  loop.run_in_loop([&] {
    ch = new en::Channel(&loop, read_fd);
    ch->set_read_callback([&] {
      char buf[64];
      const ssize_t n = ::read(ch->fd(), buf, sizeof(buf) - 1);
      if (n > 0) {
        buf[n] = '\0';
        received = buf;
        got_read.store(true);
      }
    });
    ch->enable_reading();
  });

  // 等注册完成后再写入。
  std::this_thread::sleep_for(20ms);

  const char* msg = "hello-reactor";
  CHECK(::write(write_fd, msg, std::strlen(msg)) == static_cast<ssize_t>(std::strlen(msg)));

  CHECK(wait_until([&] { return got_read.load(); }, 1000ms));
  CHECK(received == "hello-reactor");

  // 清理：在 loop 线程注销并释放 Channel。
  loop.run_in_loop([&] {
    ch->remove();
    delete ch;
    ch = nullptr;
  });
  loop.quit();
  t.join();
  ::close(read_fd);
  ::close(write_fd);
  std::cout << "  [ok] pipe 可读事件回调收到完整数据" << std::endl;
}

void test_error_on_peer_close() {
  int fds[2];
  CHECK(::pipe(fds) == 0);
  const int read_fd = fds[0];
  const int write_fd = fds[1];

  en::EventLoop loop;
  std::atomic<bool> got_error{false};

  std::thread t([&loop] { loop.run(); });

  en::Channel* ch = nullptr;
  loop.run_in_loop([&] {
    ch = new en::Channel(&loop, read_fd);
    ch->set_error_callback([&] {
      got_error.store(true);
      ch->disable_reading();
    });
    ch->enable_reading();
  });
  std::this_thread::sleep_for(20ms);

  ::close(write_fd);  // 对端关闭：read 端应收到 EPOLLHUP/EPOLLRDHUP

  CHECK(wait_until([&] { return got_error.load(); }, 1000ms));

  loop.run_in_loop([&] {
    ch->remove();
    delete ch;
    ch = nullptr;
  });
  loop.quit();
  t.join();
  ::close(read_fd);
  std::cout << "  [ok] 对端关闭触发错误回调" << std::endl;
}

void test_quit_before_run() {
  en::EventLoop loop;
  loop.quit();  // 先退出，run 应立即返回
  loop.run();
  loop.quit();  // 幂等
  std::cout << "  [ok] quit 先于 run 调用时立即返回，重复 quit 幂等" << std::endl;
}

void test_repeated_quit_from_other_thread() {
  en::EventLoop loop;
  std::thread t([&loop] { loop.run(); });
  loop.quit();
  loop.quit();  // 重复 quit 不崩溃
  t.join();
  std::cout << "  [ok] 运行中重复 quit 正常退出" << std::endl;
}

void test_run_in_loop_direct_execution() {
  en::EventLoop loop;
  std::vector<std::string> order;
  std::atomic<bool> done{false};

  std::thread t([&loop] { loop.run(); });

  loop.run_in_loop([&] {
    order.push_back("outer-begin");
    // 在 loop 线程内调用 run_in_loop：立即执行，不入队。
    loop.run_in_loop([&] { order.push_back("nested"); });
    order.push_back("outer-end");
    done.store(true);
  });

  CHECK(wait_until([&] { return done.load(); }, 1000ms));
  CHECK(order.size() == 3);
  CHECK(order[0] == "outer-begin");
  CHECK(order[1] == "nested");  // 嵌套任务先于外层后续代码执行
  CHECK(order[2] == "outer-end");

  loop.quit();
  t.join();
  std::cout << "  [ok] loop 线程内 run_in_loop 立即执行（同步语义）" << std::endl;
}

}  // namespace

int main() {
  std::cout << "reactor_test:" << std::endl;
  test_task_from_other_thread();
  test_pipe_read_event();
  test_error_on_peer_close();
  test_quit_before_run();
  test_repeated_quit_from_other_thread();
  test_run_in_loop_direct_execution();

  if (g_failures == 0) {
    std::cout << "reactor_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "reactor_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
