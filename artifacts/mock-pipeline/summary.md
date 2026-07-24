# 总结：Session 编排、RAG、队列、取消与 Mock E2E

## 今天最终运行了什么
1. **完整链路**：`client(TCP) → edge_gateway → unit_manager → session_node`
   → 固定 WAV/文本 → 真实 BM25 L0-L3 路由 →（Fake LLM）→ 分句 →
   有界队列 → Fake TTS → 1 秒 WAV 输出（demo-full.log，6 个文件）。
2. **全量测试**：CTest 27/27 通过（tests.txt），含新增：
   - tests/unit/rag_test（BM25 手算对照、四类路由）
   - tests/unit/bounded_queue_test / sentence_chunker_test / wav_reader_test
   - tests/unit/session_state_test / session_pipeline_test（取消传播、
     顽固后端晚到过滤、满队列丢弃、最小合成时长）
   - tests/e2e/session_e2e_test（三进程链四类路由 + WAV + 直连取消 +
     taskinfo/exit + SIGTERM）
   - tests/fault/session_fault_test（异常后端、在途 exit/cancel、忙时拒绝）

## 哪些是 Fake、哪些是真实
| 组件 | 类型 | 说明 |
|---|---|---|
| BM25 + L0-L3 路由 | **真实** | libs/rag，JSONL 知识库，阈值实测标定（metrics.csv） |
| 有界队列/分句/WAV 读写 | **真实** | libs/common，确定性实现 |
| Session 状态机/取消过滤 | **真实** | libs/session，(generation, request_id) 双检查 |
| ASR/LLM/TTS | Fake | 确定性测试信号：ASR 数帧、LLM 回显 token、TTS 500Hz 方波 |
| 音频内容 | 无 | Fake TTS 不产生语音，内容见 final_text；真实模型接入见板端阶段 |

## 验收对照
- 固定 WAV → Fake PCM：✅ 1 秒可验证 WAV（44 字节头 + 16000 采样）
- 四类路由走对路径：✅ l0/l1/l2/l3（metrics.csv 得分可追溯）
- Session 状态机 + 有界队列：✅ 状态轨迹断言；队列峰值 ≤ 容量
- 取消后旧数据为 0：✅ 顽固后端实测 0 帧进 sink；新请求世代隔离
- sentence chunker 与晚到消息测试：✅

## 失败在哪里
- SIGTERM EINTR abort（已修复）、演示 WAV 过短（已修复，1 秒）、
  测试子进程路径解析（已修复）、L2 查询关键词冲突（已换查询）。
- 未修复限制：网关链 cancel 排队（同步控制面），见 failures.md。

## 明天的输入（次日：WSL Mock 冻结）
- 从空构建目录 configure/build/CTest（干净构建排除缓存假象）
- 50 轮 Mock E2E（client → gateway → manager → session_node）
- 错误输入/超时/取消/关闭回归 + ASan/UBSan
- deploy/taishanpi3m 板端部署包
