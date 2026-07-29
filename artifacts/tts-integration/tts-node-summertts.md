# SummerTtsBackend 板端核验（全链路合成对照）

> 日期：2026-07-30（SummerTTS 后端接入的板端验收；
> 门禁基线见 `artifacts/upstream-baseline/upstream-baseline.md` 第三段）

## 环境变更

- 板端构建两次被满载/OOM 冻结（-j6 并行编译 Eigen -O3 模板重代码，4 GB 内存，
  dmesg 无 OOM kill 记录）后：加 2 GB swapfile（`/swapfile`，已写入
  `/etc/fstab`），构建改为 `-j1`/`-j2` 受限并行。
- 板端构建目录 `build-taishanpi3m-hw`：`-DVOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=ON`
  `-DVOXORCHESTRA_SUMMERTTS_ROOT=$HOME/upstream_tts/tts`（g++ 13.3 aarch64）。
- 调试记录：`summer_tts` 库的契约依赖初版为 PRIVATE 链接，导致消费者
  （summer_tts_test）找不到 `voxorchestra/backend/` 头；改为 PUBLIC 传播
  （契约头是公共 API），板端复编通过。

## 单元测试（summer_tts_test）

模型路径经 `VOXORCHESTRA_TTS_MODEL` 注入（`tests/unit/CMakeLists.txt` 由缓存
变量设置）。固定文本 `你好，这是语音合成测试。`：

| 断言 | 结果 |
|---|---|
| kPcm 帧总采样 = dataLen = 53248（门禁基线） | 通过 |
| 每帧 ≤ kFrameSamples=320（320 采样 = 20 ms @16 kHz），余数在末帧 | 通过 |
| 末事件 kDone，kPcm 事件数 ≥ 2 | 通过 |
| RTF | 0.517（门禁基线 ~0.62，同量级）|
| 取消：cancel 后 synthesize 无任何事件 | 通过 |
| 会话重置：新 set_event_callback 清除取消状态 | 通过 |

## 全链路（gateway → manager → tts_node）

edge_gateway(9100) → unit_manager(`--node tcp://127.0.0.1:19204`) →
tts_node(19204)。tts_node 以 `--config config/taishanpi3m/session.json`
启动，tts 段（backend=summertts / model=models/single_speaker_fast.bin /
length_scale=1.0）全配置驱动，无 CLI 覆盖。

| 项 | 值 |
|---|---|
| setup | 分配 work_id `w-0`（单节点注册，轮转路由）|
| 固定文本推理响应 | payload `{"pcm_bytes":106496,"wav_path":".../tts_86e69f32.wav"}` |
| WAV 文件 | 106540 B = 44 B 头 + 106496 B PCM；`file` 确认 Microsoft PCM, 16 bit, mono 16000 Hz |
| 与 smoke 参考逐字节对比 | `cmp` 完全一致；SHA256 双侧 `d3ef814f…c244dc200` |
| tts_node 峰值 RSS | VmHWM 419240 kB ≈ 409.4 MB（门禁基线 407.9 MB + 节点框架 ~11 MB）|
| RTF | 合成 1.813 s / 3.328 s = 0.545（热启动）|

smoke 参考：`~/upstream_tts/tts/build/tts_smoke`（门禁产物，同模型同文本
同语速复现 `ref_smoke.wav`，dataLen=53248）。`pcm_bytes = 106496 = 53248 × 2`
与 WAV 长度自洽。

## 结论

SummerTtsBackend 与 FakeTtsBackend 协议等价（kPcm 帧 + kDone），板端
tts_node 进程内 text → 16 kHz mono S16 PCM，PCM 数据与上游参考**位级一致**；
RTF < 1（实时合成有余量），峰值 RSS ~409 MB 与基线同量级，可进入会话
流水线联调。优雅退出：三进程 SIGTERM 全部退出。
