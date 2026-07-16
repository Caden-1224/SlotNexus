// 节点运行时单元测试：状态机流转、任务隔离、未知任务、超时、取消、重复 exit。
#include "voxorchestra/runtime/backends.hpp"
#include "voxorchestra/runtime/task_runtime.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace etr = voxorchestra::runtime;

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

// 每次 setup 产出独立 DelayBackend 实例的运行时。
std::unique_ptr<etr::TaskRuntime> make_delay_runtime(
    std::chrono::milliseconds delay, std::size_t max_tasks = 0) {
  return std::make_unique<etr::TaskRuntime>(
      [delay] { return std::make_shared<etr::DelayBackend>(delay); },
      max_tasks);
}

// TaskChannel 状态机：未 setup 先推理、重复 setup、taskinfo 快照。
void test_channel_state_flow() {
  etr::TaskChannel channel("w-0", std::make_shared<etr::EchoBackend>());
  CHECK(channel.state() == etr::TaskChannel::State::kNew);

  std::string out;
  CHECK(channel.inference("r-1", "hi", std::chrono::milliseconds(100), &out) ==
        etr::TaskChannel::Error::kBadState);  // 未 setup

  CHECK(channel.setup("s-1", "cfg") == etr::TaskChannel::Error::kOk);
  CHECK(channel.setup("s-2", "cfg2") == etr::TaskChannel::Error::kBadState);  // 重复

  auto info = channel.taskinfo();
  CHECK(info.state == etr::TaskChannel::State::kReady);
  CHECK(info.work_id == "w-0");
  CHECK(info.setup_payload == "cfg");
  CHECK(info.in_flight.empty());

  CHECK(channel.inference("r-2", "你好", std::chrono::milliseconds(100), &out) ==
        etr::TaskChannel::Error::kOk);
  CHECK(out == "echo:你好");
  info = channel.taskinfo();
  CHECK(info.state == etr::TaskChannel::State::kReady);
  CHECK(info.inference_count == 1);
  CHECK(info.in_flight.empty());
  std::cout << "  [ok] 状态机流转：setup/重复 setup/未 setup 推理/taskinfo 快照" << std::endl;
}

// 两个 work_id：相同 request_id 在不同任务中完全隔离。
void test_work_id_isolation_same_request_id() {
  etr::TaskRuntime runtime;  // Echo 工厂
  const auto a = runtime.setup("s-a", "cfg-a");
  const auto b = runtime.setup("s-b", "cfg-b");
  CHECK(a.error == etr::TaskChannel::Error::kOk);
  CHECK(b.error == etr::TaskChannel::Error::kOk);
  CHECK(a.work_id != b.work_id);

  std::string out_a;
  std::string out_b;
  CHECK(runtime.inference(a.work_id, "r-1", "ping-a",
                          std::chrono::milliseconds(100), &out_a) ==
        etr::TaskChannel::Error::kOk);
  CHECK(runtime.inference(b.work_id, "r-1", "ping-b",
                          std::chrono::milliseconds(100), &out_b) ==
        etr::TaskChannel::Error::kOk);
  CHECK(out_a == "echo:ping-a");
  CHECK(out_b == "echo:ping-b");

  const auto info_a = runtime.taskinfo(a.work_id);
  const auto info_b = runtime.taskinfo(b.work_id);
  CHECK(info_a.has_value() && info_b.has_value());
  CHECK(info_a->work_id == a.work_id && info_b->work_id == b.work_id);
  CHECK(info_a->inference_count == 1 && info_b->inference_count == 1);
  CHECK(info_a->in_flight.empty() && info_b->in_flight.empty());
  std::cout << "  [ok] 两个 work_id 隔离：相同 request_id 互不串扰" << std::endl;
}

// 未知任务：一切操作返回 kNotExist，不崩溃。
void test_unknown_task() {
  etr::TaskRuntime runtime;
  std::string out;
  CHECK(runtime.inference("w-99", "r-1", "x", std::chrono::milliseconds(100),
                          &out) == etr::TaskChannel::Error::kNotExist);
  CHECK(runtime.cancel("w-99", "r-1") == etr::TaskChannel::Error::kNotExist);
  CHECK(runtime.exit("w-99") == etr::TaskChannel::Error::kNotExist);
  CHECK(!runtime.taskinfo("w-99").has_value());
  CHECK(!runtime.is_alive("w-99"));
  CHECK(runtime.size() == 0);
  std::cout << "  [ok] 未知任务：inference/cancel/exit/taskinfo 均 kNotExist" << std::endl;
}

// 超时：Delay 推理超过 deadline 返回 kTimeout，之后通道恢复可继续推理。
void test_inference_timeout() {
  auto runtime = make_delay_runtime(std::chrono::milliseconds(200));
  const auto setup = runtime->setup("s-1", "cfg");
  CHECK(setup.error == etr::TaskChannel::Error::kOk);

  std::string out;
  CHECK(runtime->inference(setup.work_id, "r-1", "slow",
                           std::chrono::milliseconds(50), &out) ==
        etr::TaskChannel::Error::kTimeout);
  CHECK(out.empty());

  // 超时后回到 kReady：再推理（timeout=0 → 默认 5s）成功。
  CHECK(runtime->inference(setup.work_id, "r-2", "again",
                           std::chrono::milliseconds(0), &out) ==
        etr::TaskChannel::Error::kOk);
  CHECK(out == "echo:again");
  CHECK(runtime->taskinfo(setup.work_id)->inference_count == 2);
  std::cout << "  [ok] 超时：kTimeout 后通道恢复，可再次推理" << std::endl;
}

// 取消与单流：在途期间再次发起 kBusy；cancel 协作式中止；不影响其他任务。
void test_cancel_and_busy() {
  auto runtime = make_delay_runtime(std::chrono::milliseconds(300));
  const auto a = runtime->setup("s-1", "cfg");
  const auto b = runtime->setup("s-2", "cfg");
  CHECK(a.error == etr::TaskChannel::Error::kOk);
  CHECK(b.error == etr::TaskChannel::Error::kOk);

  std::string out;
  std::thread worker([&runtime, &out, &a] {
    runtime->inference(a.work_id, "r-1", "long",
                       std::chrono::milliseconds(2000), &out);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(80));

  // 在途期间再次发起 → kBusy（单流拒绝并发）。
  CHECK(runtime->inference(a.work_id, "r-2", "second",
                           std::chrono::milliseconds(50), &out) ==
        etr::TaskChannel::Error::kBusy);

  // 取消另一个空闲任务：空操作 kOk，不受影响。
  CHECK(runtime->cancel(b.work_id, "r-x") == etr::TaskChannel::Error::kOk);

  // 取消在途请求；worker 将协作式返回 kCancelled。
  CHECK(runtime->cancel(a.work_id, "r-1") == etr::TaskChannel::Error::kOk);
  worker.join();
  CHECK(out.empty());  // 被取消的推理无产出
  CHECK(runtime->taskinfo(a.work_id)->state == etr::TaskChannel::State::kReady);

  // 取消后任务可继续推理。
  CHECK(runtime->inference(a.work_id, "r-3", "recover",
                           std::chrono::milliseconds(0), &out) ==
        etr::TaskChannel::Error::kOk);
  CHECK(out == "echo:recover");
  std::cout << "  [ok] 取消与单流：kBusy 拒绝并发、cancel 协作式中止、不误伤他任务" << std::endl;
}

// 重复 exit：退出后一切操作 kNotExist，任务从运行时移除。
void test_repeated_exit() {
  etr::TaskRuntime runtime;
  const auto setup = runtime.setup("s-1", "cfg");
  CHECK(setup.error == etr::TaskChannel::Error::kOk);

  CHECK(runtime.exit(setup.work_id) == etr::TaskChannel::Error::kOk);
  CHECK(runtime.exit(setup.work_id) == etr::TaskChannel::Error::kNotExist);

  std::string out;
  CHECK(runtime.inference(setup.work_id, "r-1", "x",
                          std::chrono::milliseconds(100), &out) ==
        etr::TaskChannel::Error::kNotExist);
  CHECK(runtime.cancel(setup.work_id, "r-1") ==
        etr::TaskChannel::Error::kNotExist);
  CHECK(!runtime.taskinfo(setup.work_id).has_value());
  CHECK(!runtime.is_alive(setup.work_id));
  CHECK(runtime.size() == 0);
  std::cout << "  [ok] 重复 exit：第二次 kNotExist，退出后任务不可再操作" << std::endl;
}

// setup_with：外部指定 work_id（Unit Manager 全局分配场景）。
void test_setup_with_external_work_id() {
  etr::TaskRuntime runtime;
  const auto ext = runtime.setup_with("w-7", "s-1", "cfg");
  CHECK(ext.error == etr::TaskChannel::Error::kOk);
  CHECK(ext.work_id == "w-7");
  CHECK(runtime.is_alive("w-7"));

  // 同名任务重复创建 → kBadState。
  CHECK(runtime.setup_with("w-7", "s-2", "cfg2").error ==
        etr::TaskChannel::Error::kBadState);

  // 自分配 setup() 序列不受外部 id 影响。
  const auto self = runtime.setup("s-3", "c");
  CHECK(self.error == etr::TaskChannel::Error::kOk);
  CHECK(self.work_id == "w-0");

  // 按外部 id 正常推理，随后释放。
  std::string out;
  CHECK(runtime.inference("w-7", "r-1", "hi", std::chrono::milliseconds(100),
                          &out) == etr::TaskChannel::Error::kOk);
  CHECK(out == "echo:hi");
  CHECK(runtime.exit("w-7") == etr::TaskChannel::Error::kOk);
  CHECK(!runtime.is_alive("w-7"));
  std::cout << "  [ok] setup_with：外部 work_id 创建/重复拒绝/推理/释放" << std::endl;
}

// 容量：超限 setup 返回 kCapacity，exit 释放后名额恢复且 id 不复用。
void test_capacity() {
  etr::TaskRuntime runtime(2);
  CHECK(runtime.setup("s-1", "c").error == etr::TaskChannel::Error::kOk);
  CHECK(runtime.setup("s-2", "c").error == etr::TaskChannel::Error::kOk);
  CHECK(runtime.setup("s-3", "c").error == etr::TaskChannel::Error::kCapacity);

  CHECK(runtime.exit("w-0") == etr::TaskChannel::Error::kOk);
  const auto again = runtime.setup("s-4", "c");
  CHECK(again.error == etr::TaskChannel::Error::kOk);
  CHECK(again.work_id == "w-2");  // 新任务不复用已释放 id
  std::cout << "  [ok] 容量上限：kCapacity、释放后恢复、id 不回收复用" << std::endl;
}

}  // namespace

int main() {
  std::cout << "runtime_test:" << std::endl;
  test_channel_state_flow();
  test_work_id_isolation_same_request_id();
  test_unknown_task();
  test_inference_timeout();
  test_cancel_and_busy();
  test_repeated_exit();
  test_capacity();
  test_setup_with_external_work_id();

  if (g_failures == 0) {
    std::cout << "runtime_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "runtime_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
