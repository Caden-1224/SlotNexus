# RkllmBackend 板端核验（全链路生成对照）

> 日期：2026-07-31（RKLLM 后端接入的板端验收；
> 门禁基线见 `artifacts/upstream-baseline/upstream-baseline.md` 第一段）

## 环境变更

- **节点内推理超时参数化**：`TaskRuntime` 的 `kDefaultInferenceTimeout`
  （5000 ms）击穿 RKLLM 单次推理（板端 ~15 s，100 token @ ~6.5 tok/s）。
  为 `RuntimeNode` 增加构造注入 `infer_timeout`（默认 0 = 原默认 5000 ms，
  其余节点不受影响），llm_node 经 `--infer-timeout-ms` 传入；板端核验用
  60000 ms。
- **异步生成入口**：`rkllm_run`（`is_async=false` 时同步阻塞，回调由调用
  线程触发）只能让事件批量落地，流式投递与中途取消都失效；改用
  `rkllm_run_async`（回调来自厂商内部线程），配合受控队列 + 泵循环实现
  流式 kToken 投递与协作式取消。
- 模型按 tts/asr 模式 staged 到仓库 `models/`（gitignored，不入库），
  复制后 sha256sum 复核：`5f163f25…f6c5437`（与开发机源、LOCAL_RESOURCES
  三处一致）。
- 构建：`-DVOXORCHESTRA_RKLLM_ROOT=/home/lckfb/workspace/upstream_rkllm/
  rknn-llm/rkllm-runtime/Linux/librkllm_api`（含 `include/rkllm.h` 与
  `aarch64/librkllmrt.so`）；运行时 librkllmrt.so 可经链接器 RUNPATH
  解析，板端脚本仍显式 `LD_LIBRARY_PATH` 双保险。
- 新增 `deploy/taishanpi3m/run_llm_chain.sh`：gateway→manager→llm_node
  全链路核验脚本（板端执行）。

## 单元测试（rkllm_llm_test）

模型路径经 `VOXORCHESTRA_RKLLM_MODEL` 注入。固定 prompt（与门禁 smoke
同款）`你好，请用一句话介绍你自己。`，采样参数 max_new_tokens=100 /
max_context_len=256 / top_k=1 / temp=0.8 / embed_flash=1（教程参考值）：

| 断言 | 结果 |
|---|---|
| kToken 流（全部非空）+ 末事件 kDone，kDone.text = token 拼接 | 通过 |
| TTFT / tok/s（与门禁基线 288 ms / 7.79 tok/s 对照） | 339.9 ms / 6.65 tok/s（同量级）|
| 输出文本 | 与同参数 smoke 逐字一致（top_k=1 贪心，确定性）|
| 取消：cancel 后 generate 无任何事件 | 通过 |
| 生成中取消：cancel 后无新事件、无 kDone（旧 token 过滤） | 通过 |
| 会话重置：新 set_event_callback 清除取消状态 | 通过（输出逐字一致）|

## 全链路（gateway → manager → llm_node）

edge_gateway(9100, `--forward-timeout-ms 30000`) → unit_manager
(`--node tcp://127.0.0.1:19203 --node-rpc-timeout-ms 30000`) →
llm_node(19203，`--config config/taishanpi3m/session.json
--infer-timeout-ms 60000`，llm 段 backend=rkllm / model /
max_new_tokens=100 / max_context_len=256 全配置驱动，无 CLI 覆盖)。

| 项 | 值 |
|---|---|
| setup | 分配 work_id `w-0`（含 rkllm_init + 模型加载，~3 s）|
| 固定 prompt 推理响应 | payload `{"text":"Yes, I can help you with that.｜\n\nYou have a 40% chance…"}`（100 token）|
| 与 smoke 参考对照 | **逐字一致**（`rkllm_smoke <模型> <prompt> 100`：TTFT 337.4 ms / 6.42 tok/s）|
| taskinfo | state=ready，inference_count=1 |
| llm_node 峰值内存 | VmHWM 1060700 kB ≈ **1.01 GiB**（1.5B W4A16 预算 ~1.30 GiB，模型走 mmap 未吃满 RSS；门禁 free 粗测 available ~2.5 GB 一致）|

smoke 参考：`~/workspace/upstream_rkllm/smoke/rkllm_smoke`（门禁产物，
同模型 + 同 prompt + max_new_tokens=100 复现基线，输出与后端逐字一致）。

## 采样参数校准观察（不阻塞，留给会话链路）

- 输出为英文跑题（meta 式自问自答），与门禁基线观察一致：R1 推理模型 +
  100 token 上限内未产生有意义中文回复。待校准项：chat template
  （当前与 smoke 一致用 llm_demo 的纯 `｜User｜/｜Assistant｜`；模型自带
  `<｜User｜>…<｜Assistant｜><think>\n` 模板未用）、采样参数
  （temp=0.8 / top_k=1）、max_new_tokens 预算。板端实测 6.4~6.7 tok/s，
  128/256 token 需 ~20/40 s，与推理超时参数化的取值一起在会话联调时定。
- 板端 tok/s 较门禁 7.79 略降（6.42~6.65，连续运行后热降频，8 核负载
  正常）；TTFT 337~340 ms 与门禁 288 ms 同量级。

## 结论

RkllmBackend 与 FakeLlmBackend 协议等价（kToken 流 + 末事件 kDone 携带
完整输出），板端 llm_node 进程内 prompt → 流式 token → 最终文本，与上游
smoke 参考**逐字一致**（确定性输出）；取消后旧 token 全过滤、会话重置
正常；峰值内存 ~1.01 GiB 在预算内；SIGTERM 三进程优雅退出。节点内推理
超时已参数化（默认 5000 ms 不变，x86 Mock 回归不受影响）。可进入会话
流水线联调（RAG 路由 + 采样参数校准后全真实链路）。
