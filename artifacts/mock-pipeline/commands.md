# 复现命令

## 构建
```bash
cd <repo>
cmake --preset wsl-debug            # 或直接 cmake -B build-wsl
cmake --build build-wsl -j8
```

## 全量测试
```bash
cd build-wsl && ctest --output-on-failure
# 结果：27/27 通过（tests.txt）
```

## 演示（完整链路：client → gateway → manager → session_node）
```bash
bash scripts/demo_mock_session.sh
# 日志与输出：/tmp/voxorchestra-session/（6 个 1 秒 WAV）
# 本次运行完整输出：demo-full.log
```

## 固定 WAV fixture 生成
```bash
python3 scripts/gen_fixture_wav.py            # 默认 data/fixtures/voice.wav
python3 scripts/gen_fixture_wav.py /tmp/x.wav # 指定输出
```

## 硬件依赖检查
```bash
bash scripts/check_no_hw_deps.sh              # 8 个二进制均无 NPU/声卡依赖
```

## 路由得分实测（阈值标定数据）
```bash
# 临时测量程序（已删除，方法保留）：
# 知识库 8 条 → Bm25Index.build → top_k(查询, 3) 打印得分
# 结果见 metrics.csv
```
