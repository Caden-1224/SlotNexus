# backend-contracts 摘要

> 日期：2026-07-18

## 最终运行了什么

- 五类后端契约（IAsrBackend / IRetriever / ILlmBackend / ITtsBackend /
  IAudioSink）与统一事件 BackendEvent，纯头 INTERFACE 库 `voxorchestra::backend`；
- 四个确定性 Fake + WAV 音频输出（`voxorchestra::fake_backends`）：
  FakeAsrBackend / FakeRetriever / FakeLlmBackend / FakeTtsBackend / FakeAudioSink；
- 四个 Fake 节点进程（asr/rag/llm/tts）+ echo 节点，经 unit_manager 轮转路由
  与 edge_gateway 组成五节点端到端（fake_nodes_e2e_test 19/19 全绿）；
- 无硬件依赖验收：`scripts/check_no_hw_deps.sh` 通过（7 个二进制 ldd 干净）。

## 哪些是 Fake / 真实

- 五个后端全部为 **Fake（确定性模拟）**：用于测试协议、路由与编排，
  不代表真实模型能力；
- 音频输出为 **WAV 文件**（FakeAudioSink），不是声卡（声卡属板端）；
- 唯一"真实"部分是进程拓扑与消息链路本身：五节点 → Manager → 网关全为
  真实进程与真实 ZMQ/TCP 通信。

## 关键事实

| 项 | 值 |
|---|---|
| 契约 | 5 接口 + 统一事件（partial/final/token/pcm/done），零第三方依赖 |
| Fake 确定性 | 输出完全由输入决定，无随机源；取消后不再产出事件 |
| 节点 | 外壳只依赖接口，Backend 经工厂注入（可独立替换） |
| 端口约定 | echo 19200 / asr 19201 / rag 19202 / llm 19203 / tts 19204 |
| 验收 | 各 Fake 独立运行互不串扰；x86 构建无 NPU SDK/声卡依赖 |

## 失败在哪里

- asr_node 默认端口冲突缺陷已修复并记录（见 failures.md）；
- 其余步骤无失败。

## 明天的输入

- Session 编排（L0-L3 路由、JSONL/BM25、有界队列、sentence chunker、
  取消传播与晚到消息过滤），四类 Fake 将作为 Session 的后端被真实编排。
