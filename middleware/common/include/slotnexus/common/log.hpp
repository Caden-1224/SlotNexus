// 进程级日志输出（，M1 门禁 3：完整日志可按 request_id 关联）。
// Author: Caden
//
// edge_gateway / unit_manager / session_node 的请求级日志统一经此函数
// 输出：行首时间戳 + 事件字段（含 request_id）。跨进程按 request_id
// 拼接即可还原一次调用的完整路径：
//
//  gw req → mgr req / mgr alloc / mgr reply → session req / run / done
//
// 约定：请求级事件均带 request_id=<一次调用的标识>；解析失败等无法
// 关联的帧记 err bad_json（见各进程实现）。日志写 stderr，进程日志
// 统一重定向到文件（demo 脚本与测试均如此）。
#pragma once

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace slotnexus::common {

// 行首时间戳（本地时区 HH:MM:SS.mmm，单进程内即可排序）。
inline std::string LogTimestamp() {
 using Clock = std::chrono::system_clock;
 const auto now = Clock::now();
 const auto millis =
     std::chrono::duration_cast<std::chrono::milliseconds>(
         now.time_since_epoch())
         .count() %
     1000;
 const std::time_t t = Clock::to_time_t(now);
 std::tm tm{};
 localtime_r(&t, &tm);
 char buf[16];
 std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
 return std::string(buf) + "." + std::to_string(millis);
}

// 线程安全单行日志（session_node 工作线程与主线程并发调用）。
inline void LogLine(const std::string& line) {
 static std::mutex mutex;
 std::lock_guard<std::mutex> lock(mutex);
 std::fprintf(stderr, "[%s] %s\n", LogTimestamp().c_str(), line.c_str());
}

}  // namespace slotnexus::common
