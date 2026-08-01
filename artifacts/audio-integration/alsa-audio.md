# ALSA 音频接入板端核验（播放全链路 + 录音对照）

> 日期：2026-08-01（ALSA 后端接入的板端验收）
> 范围：backends/alsa（AlsaAudioSink 播放 / AlsaAudioSource 录音）、
> tts_node --sink alsa 全链路、x86 门禁回归。

## 环境变更

- 板端安装 `libasound2-dev`（1.2.11-1ubuntu0.3，arm64；`/usr/include/alsa/`
  全套头 + `/usr/lib/aarch64-linux-gnu/libasound.so.2`）。apt 直装成功，
  无需开发机兜底拷贝。
- 构建方式与三大厂商 SDK（sherpa_onnx / rkllm / summer_tts）的差异：
  后者由 `VOXORCHESTRA_*_ROOT` 缓存变量指向板端源码/SDK 目录并
  FATAL_ERROR 门控；**ALSA 是系统包，无 ROOT 变量，直接
  `find_package(ALSA REQUIRED)`**（CMake FindALSA，无 pkg-config 依赖）。
  契约头 `i_audio_sink.hpp` PUBLIC 传播，`ALSA::ALSA` PRIVATE
  （静态库 LINK_ONLY 传导，与既有后端同款）。
- 硬件构建目录 `build-taishanpi3m-hw`：既有缓存参数（Release、
  `-DVOXORCHESTRA_ENABLE_HARDWARE_BACKENDS=ON` + 三个 ROOT）基础上追加
  `-DVOXORCHESTRA_ALSA_DEVICE=plughw:0,0`（单测设备注入，缺省 default）。

## 声卡与设备路由（ES8323 / rockchipes8388）

| 项 | 值 |
|---|---|
| 播放 + 录音 | card 0 `rockchipes8388`（ES8323 codec，dailink-multicodecs）|
| 播放（仅） | card 1 rockchiphdmi / card 2 rockchipdp0 |
| amixer 路由 | Speaker `Playback [on]`，Headphone `[off]`（板载喇叭为默认输出）|
| aplay 基线 | `aplay -D plughw:0,0` 与 `-D default` 播放 16 kHz 1 s 500 Hz 正弦（gen_fixture_wav.py 产物）均 exit 0 |
| default 设备 | 板端 `default` = PulseAudio 路由（无 PA 进程时回落 plug）；`aplay -L` 确认 |

## 采样率约束与兜底（板端实测发现）

ES8323 codec 在 5644800 Hz MCLK 下**不支持 22050 / 11025 Hz**
（`snd_pcm_dai_hw_params` 返回 -EINVAL，dmesg 有记录）。实测支持率：

| 采样率 | 8000 | 11025 | 22050 | 24000 | 32000 | 44100 | 48000 |
|---|---|---|---|---|---|---|---|
| plughw:0,0 | OK | FAIL | FAIL | OK | OK | OK | OK |

SummerTTS 输出率为 22050（上游 VITS 模型率）。处理方案（对齐上游
AlsaPlay 的重采样职责）：

1. `open()` 候选率阶梯：requested → 16000 → 44100 → 48000 → 8000，
   每档重开句柄，以 `snd_pcm_hw_params` 成功为准（rate_near 在 PCM 层
   接受 22050，codec DAI 到 hw_params 才拒绝）；
2. `write_pcm()` 采样率不一致时线性插值重采样（保音高保时长，
   ~30 行，无第三方依赖；上游用带限 sinc，语音场景线性插值足够）；
3. 实际率经 `actual_sample_rate()` 上报（tts_node 返回 JSON）。

调试中发现的第二个坑：**plug 设备（default）的 `snd_pcm_hw_params_any`
返回 1（非负非 0）**，首版用 `== 0` 判定导致 default 整档误判失败；
改为 errno 惯例（负数才失败），default/plughw 双设备通过。板端
`default` 直通 22050（plug 层转换），`plughw:0,0` 走 16000 回退 + 重采样。

## 单元测试（alsa_audio_test，板端）

设备名经 `VOXORCHESTRA_ALSA_DEVICE` 注入（缓存变量，ctest ENVIRONMENT
属性；直跑缺省 default）。无模型 env；open 失败记录并跳过（无声卡不挂死）。

| 用例 | default | plughw:0,0 |
|---|---|---|
| sink open→write(1 s×2)→close 生命周期；重复 open false；未 open write false；close 幂等；close 后重开 | 通过（实际 16000 Hz）| 通过（实际 16000 Hz）|
| sink 22050 Hz 请求（SummerTTS 输出率）| 通过（实际 22050 Hz，直通）| 通过（回退 16000 Hz，线性重采样）|
| drain：100 ms 数据 close 返回 true（snd_pcm_drain 全量播完）| 通过 | 通过 |
| source open 后读 5×1600 帧：长度正确、RMS 非零、close 幂等、未 open read 空帧 | 通过（RMS 1.8）| 通过（RMS 59.9，播放中环境耦合）|

ctest 结果：`alsa_audio_test` Passed（2.2 s）。

板端硬件构建全量回归：33/33 全绿（sherpa_asr_test 31.5 s、
rkllm_llm_test 46.4 s、summer_tts_test 12.2 s、alsa_audio_test 2.2 s
及各单元/集成/e2e 用例）。注意：e2e 用例（echo/fake_nodes/chain_fault）
要求无残留节点进程占用端口 19200-19204/9100——首次全量跑在
gateway/manager 全链路残留进程未清时出现 30 s 超时失败，清理后
重跑通过（操作纪律见记忆：板端勿叠跑模型类测试，rkllm 模型
~1 GiB anon × N 实例会触发 kernel oom-kill）。

## 全链路播放（gateway → manager → tts_node，ES8323 出声）

edge_gateway(9100) → unit_manager(`--node tcp://127.0.0.1:19204`
`--node-rpc-timeout-ms 15000`) → tts_node(19204)。tts_node 以
`--config config/taishanpi3m/session.json --backend summertts
--sink alsa --sink-device <设备>` 启动（模型 models/single_speaker_fast.bin）。

固定文本 `你好，这是语音合成测试。`（与 artifacts/tts-integration/
tts-node-summertts.md 的 WAV 基线同文本）：

| 项 | plughw:0,0 路由 | default 路由 |
|---|---|---|
| 返回 payload.text | `{"device":"plughw:0,0","pcm_bytes":106496,"sample_rate":16000}` | `{"device":"default","pcm_bytes":106496,"sample_rate":22050}` |
| pcm_bytes | 106496 = 53248 × 2，与基线 WAV（tts_86e69f32.wav）逐字节一致 | 同左 |
| 实际采样率 | 16000（ES8323 不支持 22050，回退 + 重采样）| 22050（plug 层直通）|
| 播放时长（drain） | 2.42 s（22050 数据重采样至 16000，时长不变）| 2.41 s（原生）|
| 总耗时（含合成 ~1.8 s）| 3.14 s | 4.55 s |
| 听感 | 待人工补验（见下）| 同左 |

> 听感说明：泰山派 3M 无板载扬声器（官方 wiki 确认仅 3.5 mm 接口 + 板载
> 麦克风），验收当时无 3.5 mm 设备。客观佐证已齐：drain 完成（snd_pcm_drain
> 返回成功）、pcm_bytes 与既有 WAV 基线逐字节一致、aplay 双设备 exit 0、
> 录音 RMS 非零。HDMI 音频路径（`plughw:1,0`，rockchiphdmi）已实测可播放
> （aplay exit 0），补听时优先接 HDMI 显示器/电视（带喇叭）或 USB 声卡，
> 用 `--sink-device plughw:1,0` 走全链路即可。

链路超时参数说明：tts_node 播放含 drain 阻塞（合成 + 播放 > 3 s），
gateway `--forward-timeout-ms`（默认 3000）与 manager
`--node-rpc-timeout-ms`（默认 3000）需放宽到 15000（参数化既定能力，
记忆见控制面 RPC 超时参数化）。

## 录音对照（arecord vs 历史基线 board_mic.wav）

| 项 | board_mic.wav（历史基线，08-02 15:56）| alsa_rec_44k.wav（本次）|
|---|---|---|
| 格式 | 16-bit S16_LE 双声道 44100 Hz | 16-bit S16_LE 双声道 44100 Hz（同参数）|
| 时长 | 10.00 s | 4.00 s（-d 4）|
| RMS | 63.8 | 57.0（同量级，麦克风环境音）|

arecord `-D plughw:0,0 -f S16_LE -c 2 -r 44100 -d 4` exit 0；
`file` 确认 RIFF Microsoft PCM 16 bit stereo 44100 Hz，格式与基线一致。
AlsaAudioSource（16 kHz 单声道 int16，与 IAsrBackend::feed_audio 同型）
路径由单测覆盖（RMS 1.8 / 59.9）。

## 内存增量（VmHWM）

| 项 | 值 |
|---|---|
| tts_node VmHWM（default 路由全链路后）| 419428 kB ≈ 409.4 MB |
| 门禁基线（SummerTTS 模型 + 节点框架）| ~419 MB（408 MB 模型 + ~11 MB 框架）|
| ALSA sink 增量 | 可忽略（仅 period 尺寸缓冲，无模型）|

## x86 门禁回归（开发机 WSL）

默认构建（无 VOXORCHESTRA_ENABLE_HARDWARE_BACKENDS）干净构建后：
CTest 29/29 全绿（不增不减），`scripts/check_no_hw_deps.sh` 通过
（8 个可执行均无 asound/sherpa/rkllm 链接）。ALSA 仅在硬件块内
`add_subdirectory(alsa)` 与 `find_package(ALSA REQUIRED)`，默认构建
不触碰 ALSA。

## 结论

AlsaAudioSink 与 FakeAudioSink 生命周期语义逐项对齐（open/write/close
幂等、未 open 空操作、析构自动 close、drain 语义），tts_node
`--sink alsa` 板端全链路：固定文本合成 → ES8323 播放路径 drain 完成，
pcm_bytes 与既有 WAV 基线逐字节一致；22050 输出率在 ES8323 不支持时
自动回退 16000 + 线性重采样（保音高保时长）；录音格式/量级与历史
基线一致；内存与基线同量级。听感待人工补验（无 3.5 mm 设备，HDMI
路径已实测可播）。可进入会话流水线真机联调
（AlsaAudioSource → 会话输入，i_audio_source 契约按需定义）。
