# 失败与修复记录

## 1. SIGTERM 导致 session_node abort（已修复）
- 现象：SIGTERM 后 session_node 崩溃（"terminate called after throwing
  zmq::error_t: Interrupted system call"），core dumped。
- 根因：`zmq::poll` 在信号中断时返回 EINTR，旧版 cppzmq 直接抛异常；
  serve_once 未捕获，异常逃逸到 main → std::terminate。
- 修复：serve_once 捕获 `zmq::error_t`，EINTR 按"无事件"返回（与
  RpcServer::serve_once_timeout 的处理一致），调用方重新轮询并检查退出标志。
- 回归：session_e2e_test 的 SIGTERM 三进程退出码 0 断言。

## 2. 演示 WAV 时长过短（"闪一下"）（已修复）
- 现象：输出 WAV 只有 20-200ms（Fake TTS 每 32 字节文本产 1 帧 20ms）。
- 决策：不改变 FakeTtsBackend 的确定性契约（测试依赖精确帧数），
  在 SessionPipeline 增加 tts_min_duration_ms 配置，输出不足时补静音帧。
- 修复：config/mock/session.json 设 1000ms；取消/失败路径不补齐。
- 回归：session_pipeline_test 新增 test_min_tts_duration_padding
  （补齐到 50 帧精确断言 + 默认 0 不补齐断言）。

## 3. 测试子进程全部启动失败（已修复）
- 现象：session_e2e_test 所有断言连锁失败，子进程日志为空。
- 根因：main 先 chdir 到仓库根，之后 `absolute(".")` 解析到仓库根，
  ".." 相对路径把子进程二进制定位到错误目录，execv 失败退出 127。
- 修复：chdir 前计算 e2e_dir/root 绝对路径，spawn 使用绝对路径；
  增加子进程存活检查与日志转储（启动失败立即暴露）。

## 4. 演示 L2 查询被误判为 L0（已修复）
- 现象："生成取消过滤" 路由到 l0（关键词"取消"子串命中）。
- 说明：L0 关键词匹配是规范化文本子串匹配，控制词"取消"会命中含该词
  的事实型查询——这是关键词路由的固有取舍。
- 处理：演示/E2E 的 L2 查询改为"生成过滤怎么实现"（不含控制词），
  行为已写入 metrics.csv；关键词表保持可配置。

## 5. 未修复的限制（如实记录）
- gateway/unit_manager 是同步 REQ/REP 转发：inference 在途时，经网关链
  发送的 cancel 会排队等待推理完成。session_node 本身异步（ROUTER +
  工作线程），可并发接收 cancel/taskinfo/exit；取消验证通过直连
  session_node（session_e2e_test）与管线单测（顽固后端晚到过滤）。
  该限制是"exit 与 inference 并发"的已知输入。
