#include "voxorchestra/runtime/task_channel.hpp"

#include <utility>

namespace voxorchestra::runtime {

TaskChannel::TaskChannel(std::string work_id, std::shared_ptr<IBackend> backend)
    : work_id_(std::move(work_id)), backend_(std::move(backend)) {}

TaskChannel::~TaskChannel() = default;

TaskChannel::Error TaskChannel::setup(const std::string& /*request_id*/,
                                      const std::string& payload) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == State::kTerminated) {
    return Error::kNotExist;
  }
  if (state_ != State::kNew) {
    return Error::kBadState;
  }
  state_ = State::kReady;
  setup_payload_ = payload;
  return Error::kOk;
}

TaskChannel::Error TaskChannel::inference(const std::string& request_id,
                                          const std::string& payload,
                                          std::chrono::milliseconds timeout,
                                          std::string* out_text,
                                          const EventSink& events) {
  if (timeout.count() <= 0) {
    timeout = kDefaultInferenceTimeout;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::kTerminated) {
      return Error::kNotExist;
    }
    if (state_ == State::kNew) {
      return Error::kBadState;
    }
    if (state_ == State::kBusy) {
      return Error::kBusy;
    }
    state_ = State::kBusy;
    in_flight_ = request_id;
    cancel_flag_.store(false);
  }

  const BackendResult result =
      backend_->infer(payload, deadline, cancel_flag_, events);

  std::lock_guard<std::mutex> lock(mutex_);
  ++inference_count_;
  if (state_ != State::kTerminated) {
    // exit 可能已在推理期间终止任务：不复活已终止的通道。
    state_ = State::kReady;
    in_flight_.clear();
  }
  if (result.code == BackendResult::Code::kOk && out_text != nullptr) {
    *out_text = result.text;
  }
  switch (result.code) {
    case BackendResult::Code::kOk:
      return Error::kOk;
    case BackendResult::Code::kCancelled:
      return Error::kCancelled;
    default:
      return Error::kTimeout;
  }
}

TaskChannel::Error TaskChannel::cancel(const std::string& /*request_id*/) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == State::kTerminated) {
    return Error::kNotExist;
  }
  if (state_ == State::kBusy) {
    cancel_flag_.store(true);  // 协作式：后端循环中检测后尽快返回
  }
  return Error::kOk;
}

TaskChannel::Error TaskChannel::exit() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == State::kTerminated) {
    return Error::kNotExist;
  }
  if (state_ == State::kBusy) {
    cancel_flag_.store(true);  // 让在途推理尽快结束
  }
  state_ = State::kTerminated;
  return Error::kOk;
}

TaskChannel::TaskInfo TaskChannel::taskinfo() const {
  std::lock_guard<std::mutex> lock(mutex_);
  TaskInfo info;
  info.state = state_;
  info.work_id = work_id_;
  info.in_flight = in_flight_;
  info.setup_payload = setup_payload_;
  info.inference_count = inference_count_;
  return info;
}

TaskChannel::State TaskChannel::state() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

const char* to_string(TaskChannel::State state) {
  switch (state) {
    case TaskChannel::State::kNew:
      return "new";
    case TaskChannel::State::kReady:
      return "ready";
    case TaskChannel::State::kBusy:
      return "busy";
    default:
      return "terminated";
  }
}

const char* to_string(TaskChannel::Error error) {
  switch (error) {
    case TaskChannel::Error::kOk:
      return "ok";
    case TaskChannel::Error::kNotExist:
      return "not_exist";
    case TaskChannel::Error::kBadState:
      return "bad_state";
    case TaskChannel::Error::kBusy:
      return "busy";
    case TaskChannel::Error::kTimeout:
      return "timeout";
    case TaskChannel::Error::kCancelled:
      return "cancelled";
    default:
      return "capacity";
  }
}

}  // namespace voxorchestra::runtime
