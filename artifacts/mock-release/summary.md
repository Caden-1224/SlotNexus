# WSL Mock 冻结（M1 门禁）总结

> 结论：**四项门禁全部通过**，WSL Mock 冻结候选成立，可进入板端部署
> （`deploy/taishanpi3m/`）与板卡阶段。

## 门禁逐项结论与证据

| 门禁 | 判定 | 证据 |
|---|---|---|
| 1. 50 轮 Mock E2E 零跨流 | ✅ | `e2e-50-summary.json`：`cross_stream=0`、50/50 成功、`gate_m1: pass`（client → gateway → manager → session_node 全链轮转 l0/l1/l2/l3/wav 各 10 轮） |
| 2. 干净进程退出、无残留 | ✅ | rounds 测试收尾段与 `process-cleanup.txt`：三进程 SIGTERM 退出码 0；`/proc` 无残留进程、`/proc/net/tcp` 无 LISTEN 残留；ASan/TSan 下亦无 fd/线程异常 |
| 3. 完整日志可按 request_id 关联 | ✅ | rounds 测试日志关联 50/50×3（gateway/manager/session_node）；`session req/run/done`、`mgr req/alloc/reply/err`、`gw req/reply/err` 全事件带 `request_id=`；错误分支（bad_json/unknown_work_id/busy）同样可关联 |
| 4. 干净构建、排除旧缓存假通过 | ✅ | `clean-build.txt`：空目录 `build-clean` 全新配置 + 全量构建 29/29 CTest 通过；`ctest.txt` 附测试清单；`scripts/check_no_hw_deps.sh` 逐二进制 ldd 确认默认构建无 NPU SDK/声卡链接 |

## 冻结期间发现并修复的缺陷

| 缺陷 | 来源 | 修复 |
|---|---|---|
| Channel 事件分发 UAF：连接在回调中销毁，`handle_events` 继续访问已释放对象 | ASan（channel.cpp:26） | tie guard：`set_tie` + 分发入口 `tie_.lock()` 保活 |
| 事件循环批处理中销毁连接：`handle_wakeup` 中途执行任务导致 `active` 容器迭代失效 | TSan | `queue_in_loop` 只追加 + 批次末尾排空 + 连接 keep-alive |
| `RpcServer::close()` 跨线程销毁 zmq socket 与 `serve` 线程 recv 竞争 | TSan | `closed_` 原子化；close 只置标志，socket 由服务线程回收 |
| 非法 JSON 输入导致网关进程 terminate：解析诊断内嵌非 UTF-8 字节，`dump()` 默认严格校验抛 type_error.316 未捕获 | 故障注入回归（chain_fault_test） | `to_json()` 改用 `error_handler_t::replace`，非法输入回结构化错误而非崩溃 |
| 测试隔离：跨测试 ZMQ 重连注入（echo/fake_nodes 请求混入 rounds 会话） | 回归排查 | rounds 与故障注入测试使用独立端口（9112/19111/19211、9121/19121/19221） |
| TSan 误报甄别：`BoundedQueue` "double lock/data race" 报告 | TSan | 最小复现 `tsan-min-repro.cpp` 证明为 gcc 11.4 + 内核 6.6 运行时缺陷，非代码问题 |

## 回归规模

- 全量 CTest **29 项全部通过**（build-wsl 与 build-clean 双构建）。
- 新增覆盖：50 轮轮换回归、挂起后端 max_run 兜底超时（管线层/节点层）、
  重复 cancel/exit、链路级故障注入（非法 JSON、超长帧、未知 work_id、
  错误输入后进程可正常退出）。
- Sanitizer：ASan/UBSan 全量 29/29；TSan 独立构建全量通过
  （`setarch x86_64 -R` 规避内核 6.6 ASLR 与 gcc-11 TSan 的
  "unexpected memory mapping"，抑制文件仅覆盖 libzmq 未插桩噪声与
  EdgeGateway 延迟释放的 context）。


## 已知边界（如实记录）

- max_run 兜底超时覆盖**有限挂起**的后端（生成返回后在阶段边界回收）；
  永不返回的后端会阻塞工作线程——属板卡阶段真后端集成时的显式风险项。
- 取消传播在控制面（gateway/manager 同步转发）下直连 session_node 验证；
  板卡阶段接入异步事件通道后需重新验证取消路径。
- 本阶段音频输入输出均为 WAV 文件；板载麦克风与 3.5 mm 输出属板卡阶段。
- 性能数据（延迟、吞吐、RSS 峰值）未在本阶段采集，待板端基线验证后补充。
