// MessageEnvelope 单元测试：序列化往返、版本/类型/长度校验、错误路径。
#include "voxorchestra/protocol/message_envelope.hpp"

#include <cassert>
#include <iostream>
#include <sstream>
#include <string>

namespace eflow = voxorchestra::protocol;

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

#define CHECK_THROWS_CODE(expr, expected_code)                               \
  do {                                                                       \
    bool caught = false;                                                     \
    try {                                                                    \
      expr;                                                                  \
    } catch (const eflow::ProtocolError& e) {                                \
      caught = true;                                                         \
      if (e.code() != (expected_code)) {                                     \
        ++g_failures;                                                        \
        std::cerr << "FAIL " << __FILE__ << ":" << __LINE__                  \
                  << ": 错误码不符，期望 " << static_cast<int>(expected_code) \
                  << " 实际 " << static_cast<int>(e.code()) << std::endl;    \
      }                                                                      \
    } catch (...) {                                                          \
      ++g_failures;                                                          \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__                    \
                  << ": 抛出非 ProtocolError" << std::endl;                  \
    }                                                                        \
    if (!caught) {                                                           \
      ++g_failures;                                                          \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__                    \
                  << ": 未抛出异常: " << #expr << std::endl;                  \
    }                                                                        \
  } while (0)

void test_round_trip() {
  eflow::MessageEnvelope env;
  env.set_work_id("w-1");
  env.set_request_id("r-1");
  env.set_session_id("s-1");
  env.set_type(eflow::MessageType::kInference);
  env.set_index(3);
  env.set_timestamp_ms(1234567890);
  env.set_payload({{"text", "你好"}});
  env.set_finish(false);

  const std::string json = env.to_json();
  const eflow::MessageEnvelope back = eflow::MessageEnvelope::from_json(json);

  CHECK(back.version() == eflow::kProtocolVersion);
  CHECK(back.work_id() == "w-1");
  CHECK(back.request_id() == "r-1");
  CHECK(back.session_id() == "s-1");
  CHECK(back.type() == eflow::MessageType::kInference);
  CHECK(back.index() == 3);
  CHECK(back.timestamp_ms() == 1234567890);
  CHECK(back.payload().at("text") == "你好");
  CHECK(!back.finish());
  CHECK(back.error().empty());
  std::cout << "  [ok] 全字段往返一致" << std::endl;
}

void test_minimal_envelope() {
  eflow::MessageEnvelope env;
  env.set_type(eflow::MessageType::kEvent);

  const std::string json = env.to_json();
  const eflow::MessageEnvelope back = eflow::MessageEnvelope::from_json(json);

  CHECK(back.work_id().empty());
  CHECK(back.index() == -1);
  CHECK(back.timestamp_ms() == -1);
  CHECK(back.payload().empty());
  CHECK(!back.finish());
  std::cout << "  [ok] 最小信封默认值正确" << std::endl;
}

void test_error_round_trip() {
  eflow::MessageEnvelope env;
  env.set_type(eflow::MessageType::kError);
  env.set_error({42, "任务不存在"});

  const eflow::MessageEnvelope back =
      eflow::MessageEnvelope::from_json(env.to_json());

  CHECK(!back.error().empty());
  CHECK(back.error().code == 42);
  CHECK(back.error().message == "任务不存在");
  std::cout << "  [ok] 结构化错误往返一致" << std::endl;
}

void test_bad_json() {
  CHECK_THROWS_CODE(eflow::MessageEnvelope::from_json("not json at all"),
                    eflow::ProtocolErrorCode::kInvalidJson);
  CHECK_THROWS_CODE(eflow::MessageEnvelope::from_json("[1,2,3]"),
                    eflow::ProtocolErrorCode::kInvalidJson);
  std::cout << "  [ok] 非法 JSON 被拒绝" << std::endl;
}

void test_unknown_version() {
  const std::string json = R"({"version": 999, "type": "event"})";
  CHECK_THROWS_CODE(eflow::MessageEnvelope::from_json(json),
                    eflow::ProtocolErrorCode::kUnknownVersion);
  std::cout << "  [ok] 未知协议版本被拒绝" << std::endl;
}

void test_invalid_type() {
  const std::string json = R"({"version": 1, "type": "teleport"})";
  CHECK_THROWS_CODE(eflow::MessageEnvelope::from_json(json),
                    eflow::ProtocolErrorCode::kInvalidType);
  std::cout << "  [ok] 未知消息类型被拒绝" << std::endl;
}

void test_missing_type() {
  const std::string json = R"({"version": 1, "work_id": "w-1"})";
  CHECK_THROWS_CODE(eflow::MessageEnvelope::from_json(json),
                    eflow::ProtocolErrorCode::kMissingField);
  std::cout << "  [ok] 缺少 type 被拒绝" << std::endl;
}

void test_oversized() {
  // 解码端：超过 1 MiB 的输入直接拒绝。
  std::string huge(eflow::kMaxSerializedBytes + 1, 'x');
  CHECK_THROWS_CODE(eflow::MessageEnvelope::from_json(huge),
                    eflow::ProtocolErrorCode::kOversized);

  // 编码端：payload 过大导致序列化超限。
  eflow::MessageEnvelope env;
  env.set_type(eflow::MessageType::kEvent);
  env.set_payload(nlohmann::json{{"blob", std::string(eflow::kMaxSerializedBytes, 'x')}});
  CHECK_THROWS_CODE(env.to_json(), eflow::ProtocolErrorCode::kOversized);
  std::cout << "  [ok] 超长消息在编解码两端都被拒绝" << std::endl;
}

void test_type_name_mapping() {
  eflow::MessageType t{};
  CHECK(eflow::message_type_from_string("setup", t) && t == eflow::MessageType::kSetup);
  CHECK(eflow::message_type_from_string("inference", t) && t == eflow::MessageType::kInference);
  CHECK(eflow::message_type_from_string("cancel", t) && t == eflow::MessageType::kCancel);
  CHECK(eflow::message_type_from_string("taskinfo", t) && t == eflow::MessageType::kTaskInfo);
  CHECK(eflow::message_type_from_string("exit", t) && t == eflow::MessageType::kExit);
  CHECK(eflow::message_type_from_string("event", t) && t == eflow::MessageType::kEvent);
  CHECK(eflow::message_type_from_string("ack", t) && t == eflow::MessageType::kAck);
  CHECK(eflow::message_type_from_string("error", t) && t == eflow::MessageType::kError);
  CHECK(!eflow::message_type_from_string("nope", t));
  CHECK(eflow::message_type_to_string(eflow::MessageType::kSetup) == "setup");
  std::cout << "  [ok] 类型字符串映射正确" << std::endl;
}

}  // namespace

int main() {
  std::cout << "protocol_test:" << std::endl;
  test_round_trip();
  test_minimal_envelope();
  test_error_round_trip();
  test_bad_json();
  test_unknown_version();
  test_invalid_type();
  test_missing_type();
  test_oversized();
  test_type_name_mapping();

  if (g_failures == 0) {
    std::cout << "protocol_test 全部通过" << std::endl;
    return 0;
  }
  std::cerr << "protocol_test 失败 " << g_failures << " 项" << std::endl;
  return 1;
}
