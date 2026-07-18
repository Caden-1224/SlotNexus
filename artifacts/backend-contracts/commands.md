# backend-contracts 执行命令

## 构建与全量测试

```bash
cd ~/work/voxorchestra-runtime
cmake --preset wsl-debug
cmake --build --preset wsl-debug -j8
ctest --test-dir build-wsl --output-on-failure
```

结果：19/19 通过（2026-07-18）。

## 单元测试（按后端）

```bash
ctest --test-dir build-wsl -R backend_contract_test --output-on-failure
ctest --test-dir build-wsl -R "fake_asr_test|fake_retriever_test|fake_llm_test|fake_tts_test|fake_audio_sink_test" --output-on-failure
```

## 五节点端到端

```bash
ctest --test-dir build-wsl -R fake_nodes_e2e_test --output-on-failure
# 或直接运行（可观察子进程日志）：
cd build-wsl/tests/integration && ./fake_nodes_e2e_test
```

## 无硬件依赖验收

```bash
scripts/check_no_hw_deps.sh
# 逐二进制 ldd，grep rkllm|rknn|sherpa|onnx|summer|asound|libsndfile
```

## 单节点冒烟（开发期间逐节点验证）

```bash
./build-wsl/apps/asr_node/asr_node &   # 19201，{"text":"3"} → 三帧连接文本
./build-wsl/apps/rag_node/rag_node &   # 19202，Top-K 可配
./build-wsl/apps/llm_node/llm_node &   # 19203
./build-wsl/apps/tts_node/tts_node &   # 19204，WAV 输出
python3 scripts/gateway_probe.py 9100 '{"version":1,"type":"setup","request_id":"s-1"}'
```
