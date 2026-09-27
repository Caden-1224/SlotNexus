# 上游基线核验记录

核验日期：2026-07-28
核验方式：开发机只读校验（文件名 / 大小 / SHA256 / 版本 / 许可证）
资源来源：开发机模型源文件
纪律依据：agent_docs/LOCAL_RESOURCES.md（版本链不混用、模型复制后必复核、Demo 不过不接 Adapter）

## 版本链总览

| 组件 | 版本 | 来源 | 核验 |
|---|---|---|---|
| RKLLM SDK | v1.2.0 | airockchip/rknn-llm release v1.2.0 | 文件+哈希已核 |
| DeepSeek-R1-Distill-Qwen-1.5B | W4A16, RK3576 | 开发机模型源（作者提供） | SHA256 与 LOCAL_RESOURCES 一致 |
| sherpa-onnx Zipformer | small bilingual zh-en 2023-02-16 | sherpa-onnx 官方 + 作者仓库 | 文件+哈希已核 |
| SummerTTS | vits-based, MIT | 作者仓库 | 文件+哈希+许可已核 |

## 1. RKLLM 版本链（v1.2.0，候选运行链）

SDK：airockchip/rknn-llm v1.2.0（README/CHANGELOG 确认，支持 RK3576 与 DeepSeek-R1-Distill）。

Runtime（aarch64），位于 `llm/rknn-llm/rkllm-runtime/Linux/librkllm_api/`：

| 文件 | 大小(B) | SHA256 |
|---|---|---|
| `aarch64/librkllmrt.so` | 7075128 | 4a23f107cd82481670beba3d0fcf9cbdb767793554df90f2060ed0de4cb000ea |
| `include/rkllm.h` | 13198 | 1c4d5ad269e98dfdcd5093bd9c85ddb68b1fe2e0c5635eef8df7c125e1e924f4 |

模型（开发机源，复制到板端后须 `sha256sum` 复核）：

| 文件 | 大小(B) | SHA256 | 与文档 |
|---|---|---|---|
| `DeepSeek-R1-Distill-Qwen-1.5B_w4a16_RK3576.rkllm` | 1394162444 | 5f163f25a548803b6ccbf9d4cb477e52f1ee9319ea44630ab70812314f6c5437 | 一致 |

Demo：`examples/DeepSeek-R1-Distill-Qwen-1.5B_Demo/`
- `deploy/src/llm_demo.cpp`（10634 B）、`deploy/CMakeLists.txt`、`deploy/build-linux.sh`
- `export/`（export_rkllm.py / generate_data_quant.py / data_quant.json，量化用，板端推理不需要）
- 运行：`./llm_demo <model> 2048 4096`（max_context / max_tokens）；`LD_LIBRARY_PATH=./lib`；`RKLLM_LOG_LEVEL=1` 采性能
- 定频：`scripts/fix_freq_rk3576.sh`（RK3576 专用脚本已确认存在）

板端 driver：内核 6.1.99，dmesg 显示 RKNPU 驱动已加载（`27700000.npu`，soc version=0 speed=5）；用户态节点 `/dev/rknpu*` 体检未出现，Demo 加载时实测确认（见 board-versions.txt）。

## 2. sherpa-onnx ASR（流式 Zipformer 中英双语 small）

模型目录：`voice/models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16/`
板端内存受限，默认用 int8 版本：

| 文件 | 大小(B) | SHA256 |
|---|---|---|
| `encoder-epoch-99-avg-1.int8.onnx` | 42980793 | db6f51551762e40e549166fe041ea3e45464370b595e9ad23f06478ec3794fbb |
| `decoder-epoch-99-avg-1.int8.onnx` | 3486740 | 4b618d383af304cfae281dbf0a53e8bf442c2f0502256cd5694bd6567ebdd834 |
| `joiner-epoch-99-avg-1.int8.onnx` | 3228485 | bdda356d6f9b8c2d7cee9ee0e26075fa537490f7fd06520be408d287073667b9 |
| `tokens.txt` | 62574 | cba59a352b6f0b99c36a7b4ea7a6a210d2270cb3e5b5d8556d98277213d5f709 |
| `bpe.model` | 244836 | bcae393dbc5611be5ffa4c7ae0841558978a5a4f484008cb9dff3a2cc97ebe01 |

fp32 版本同目录并存（encoder 88MB / decoder 13MB / joiner 12MB），板端默认不用。
`test_wavs/`：0/1/2/3/4/46.wav（固定测试音频）。
sherpa-onnx 源码：`voice/sherpa-onnx/`（含 c-api-examples、cxx-api-examples、CMakeLists）。

## 3. SummerTTS（vits 算法，MIT License）

源码：`tts/`（依赖 Eigen 3.4.0，无外部 NN 运行时；`cmake .. && make` 产物 `tts_test`）。
许可证：MIT（2024-12-14 声明，见 `tts/README.md`）。
合成接口：`int16_t* infer(const string& line, int32_t sid, float lengthScale, int32_t& dataLen)`。
运行：`./tts_test text.txt models/single_speaker_fast.bin out.wav`。

| 文件 | 大小(B) | SHA256 |
|---|---|---|
| `single_speaker_fast.bin` | 80050316 | 87b774813258a3636b3f1d9adddd3652ede503b4e8f4eb3c39065cba2a4951c4 |

模型获取方式见 `tts/README.md`（不入库，不记录网盘链接）。

## 待确认 / 风险

- **libomp.so**：rknn-llm/runtime 全树未找到。README 警告 Demo 运行时可能报 libomp.so 缺失，
  届时需从交叉编译工具链补到板端 lib 目录（与 librkllmrt.so 同级）。板端实测确认。
- **/dev/rknpu\* 设备节点**：板端体检未出现（见 board-versions.txt），RKLLM Runtime 经此节点
  访问 NPU；Demo 加载若失败需排查 BSP 用户态驱动 / udev 规则。
- **版本链纪律**：作者仓库 v1.2.0 资源为一条完整候选链，不与 Model Zoo 1.2.3 混用
  （TAISHANPI_DEPLOYMENT_REFERENCE 第 5 节）。
- **4 GB 内存**：DeepSeek 1.5B 峰值 RSS 需实测；ASR / LLM / TTS 同时驻留可能紧张，
  保留基线证据后按实测决定是否回退更小文本模型。

## RKLLM Demo 门禁结果（2026-07-28，板端实测）

板端传输后 `sha256sum` 复核：`5f163f25…f6c5437`（与开发机源、LOCAL_RESOURCES 记录三处一致）。
冒烟 Demo：`upstream_rkllm/smoke/rkllm_smoke`（基于 `llm_demo.cpp` 的 RKLLM 调用逻辑，
剥离 ZMQ 集成层，固定 prompt 直接推理）。`librkllmrt.so` 动态依赖仅 `libgomp.so.1`
（板卡 gcc 13.3 自带），**无 libomp 缺失**。

| 项 | 值 |
|---|---|
| `rkllm_init` | **success**（版本链核心门禁通过）|
| prompt | 你好，请用一句话介绍你自己。 |
| max_new_tokens | 64 |
| TTFT | 288.33 ms |
| total | 8.38 s |
| decode | 63 token / **7.79 tok/s** |
| 内存 available（free 前后）| 2587 → 2591 MB（模型走 mmap，未吃满进程 RSS）|

结论：`librkllmrt.so` v1.2.0 + `DeepSeek-R1-Distill-Qwen-1.5B_w4a16_RK3576.rkllm` + 板端
RKNPU driver（内核 6.1.99）这条版本链可加载、可流式推理，Demo 门禁通过，可进入 `RkllmBackend` 实现。

观察 / 待优化（不阻塞门禁，留给 Adapter 接入）：
- 64 token 内输出为英文跑题，未产生有意义中文回复；R1 推理模型 + 64 token 上限不足，
  chat template（`｜User｜`/`｜Assistant｜`）与采样参数（temp=0.8）需在 Adapter 校准。
- 7.79 tok/s 偏保守（仅启用 CPU0|CPU2 两核，沿用 llm_demo 默认）；后续可试 4 大核 A72 提速。
- 精确峰值 RSS 待 `/proc/<pid>/status::VmHWM` 采样；free 粗测显示内存余量充足（available ~2.5 GB）。

## sherpa-onnx ASR Demo 门禁结果（2026-07-28，板端实测）

库：板端源码编译 `libsherpa-onnx-c-api.so`（4.27 MB）+ onnxruntime aarch64（13 MB，
onnxruntime-linux-aarch64-glibc2_17 v1.17.1，SHA256 `6e0e6898…6d4fb` 与 cmake 记录一致）。
板卡完全无外网：onnxruntime zip 经开发机 hf-mirror 下载后 scp 到 `/tmp`（cmake 的
possible_file_locations 自动识别，免联网编译）。make 整体 exit 2 仅因 `sherpa-onnx-microphone-test1`
（ALSA 麦克风 example）失败，核心库 sherpa-onnx-c-api 不受影响。

模型：`sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16`（int8）。
冒烟 Demo：`upstream_rkllm/smoke/sherpa_asr_smoke`（基于 `c-api-examples/streaming-zipformer-c-api.c`，
改为命令行传模型目录 + WAV + RTF 计时）。`SherpaOnnxOnlineStreamAcceptWaveform` 接受 `float[-1,1]`，
`IAsrBackend` 的 int16 PCM 需 `/32768.0f` 归一化。

test_wavs/0.wav（10.05 s，中英混合日期句）：

| num_threads | RTF | final 文本 |
|---|---|---|
| 2 | 1.108 | 昨天是 MONDAY TODAY IS THE AFTER TOMORROW是星 |
| 4 | 1.066 | 昨天是 MONDAY TODAYS TOMORROW是星 |

结论：`libsherpa-onnx-c-api.so` + int8 模型链路可流式识别（partial 渐进产出、final 落定，
中英双语切换正确），Demo 门禁通过，可进入 `SherpaAsrBackend` 实现。RTF ~1.07（4 threads）
略 > 1 作为基线；流式 partial + 端点检测下用户体验接近实时。

待优化（不阻塞）：small int8 模型对长句有错字（TODAYS / THE AFTER），需要时可换更大模型；
RTF 压到 1 以下受限于 A72 内存带宽，线程数已饱和，需权衡 RSS 与模型档位。

## SummerTTS Demo 门禁结果（2026-07-28，板端实测）

源码：作者仓库 `tts/`（vits-based, MIT License，仅依赖 Eigen 3.4.0 自带，无外部 NN 运行时）。
作者原版 `tts/CMakeLists.txt` 已是 ZMQ+ALSA+PortAudio 集成版（产物 `tts_server`），
`test/main.cpp` 全量注释；为门禁构建纯推理冒烟程序。

冒烟源码归档：`modules/voice/tools/upstream-probes/tts_smoke.cpp` + `CMakeLists.smoke.txt`。
板端构建产物：`~/upstream_tts/tts/build/tts_smoke`（18 MB）。`CMakeLists.smoke.txt`
复刻作者 CMakeLists 的精确编译单元（glog/gflags/openfst 前端 + engipa/hz2py 文本前端 +
nn_op 算子 + modules/models vits 实现），剥离 ZMQ/ALSA/PortAudio 链接，目标改为 `tts_smoke`。
两处 GCC 13（板端 Ubuntu 24.04, g++ 13.3.0）适配，**不改上游源码**：

- `pinyinmap.h` / `hanzi2phoneid.h` / `Hanz2Piny.h` 用 `uint16_t` 未自给自足 `<cstdint>`，
  全局 `-include cstdint` 补齐。
- `engipa/ipa.cpp`（定义全局 `const wchar_t *Ipa[]`）作者原 CMakeLists 漏入，链接报
  `undefined reference to Ipa`，补入该单元即可。

WAV 输出 44 字节头（RIFF/WAVE/fmt /PCM/16-bit/mono/16000 Hz/data），与 `SummerTtsBackend`
目标产物格式一致（16 kHz 单声道 S16 PCM）。`file` 确认 `Microsoft PCM, 16 bit, mono 16000 Hz`。

固定文本 `你好，这是语音合成测试。`（5 次取样）：

| 项 | 值 |
|---|---|
| `dataLen` | 53248 samples |
| 音频时长 | 3.328 s |
| infer 耗时 | 1.87 ~ 2.08 s |
| RTF | 0.562 / 0.619 / 0.620 / 0.601 / 0.626（热启动稳定 ~0.62）|
| 模型 load（`ttsLoadModel` 读文件）| ~0.1 s |
| 峰值 RSS（`/proc/<pid>/status::VmHWM`）| 407.9 MB 稳定 |

中英混合文本 `Hello 你好，this is a mixed test 混合测试。`：dataLen=77056（4.816 s），
infer=1.932 s，RTF=0.401；engipa（英文）与 hz2py（中文）两条文本前端路径并行均走通。

结论：SummerTTS vits 链路可加载、可合成 16 kHz mono S16 PCM WAV，Demo 门禁通过，
可进入 `SummerTtsBackend` 实现。RTF ~0.62 < 1（实时合成有余量）；峰值 RSS 407.9 MB，
与 RKLLM（available ~2.5 GB）同驻需在 Backend 适配时复核内存预算。
