# 板端全真实依赖恢复记录（Qwen3.5-0.8B + MeloTTS）

> 日期：2026-09-26（WSL 侧记录，板端实测）
> 目的：板端 `~/workspace/upstream_rkllm` 依赖根在本轮开始前已不存在，
> 本文记录为“重跑当前 Qwen3.5-0.8B 全真实链路”而恢复的依赖、来源、
> 校验值，以及恢复过程中遇到的可复现网络约束。

## 1. 恢复前的板端状态

| 项 | 状态 |
|---|---|
| 板端可达性 | `ssh lckfb@10.30.165.26` 可达（本机 eth2，10.30.165.0/24） |
| `~/workspace/slotnexus-runtime` | 存在源码副本，但无 `.git`、无 `models/`、无 `build-*` |
| `~/workspace/upstream_rkllm/` | **不存在**（上一代全链路基线用的 sherpa-onnx / SummerTTS / 模型均缺失） |
| 板端保留的依赖 | `~/workspace/nexweave-board/deps/`（ONNX Runtime 1.22.1、RKNN、RKLLM 1.3.0 头/库）与 `~/rkllm_demo/`（Qwen3.5、MeloTTS、RKNN zipformer） |
| 结论 | 全链路无法直接重跑；ASR 与 TTS 依赖必须先恢复 |

## 2. 恢复后的依赖根

| 组件 | 路径 | 版本/说明 |
|---|---|---|
| RKLLM Runtime | `~/workspace/upstream_rkllm/rknn-llm/rkllm-runtime/Linux/librkllm_api/` | 1.3.0（`include/rkllm.h` + `aarch64/librkllmrt.so`） |
| sherpa-onnx | `~/workspace/upstream_rkllm/sherpa-root/` | v1.13.8 官方 linux-aarch64 shared-cpu 预编译发布件 |
| ASR 模型 | `~/workspace/upstream_rkllm/models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16/` | int8 streaming zipformer |
| MeloTTS 依赖 | `~/workspace/upstream_melotts/{include,lib,resources,models}/` | ONNX Runtime 1.22.1 + RKNN，词典/token/g 向量 |
| LLM 模型 | `~/rkllm_demo/qwen3.5-0.8b/Qwen3.5-0.8B_w4a16_g128_rk3576.rkllm` | 1,125,915,612 B |

`~/workspace/slotnexus-runtime/models/` 用符号链接指向上述模型，配置里的相对路径
（`config/taishanpi3m/session.json`）因此可直接解析。

## 3. 校验值（板端 `sha256sum` 实测）

| 文件 | 大小(B) | SHA256 |
|---|---|---|
| `librkllmrt.so`（1.3.0） | 7,617,472 | `6a9e4fc5324c68921c3a900340361e107af7599fe34dc8fa7759b2c5ae22a6e6` |
| `Qwen3.5-0.8B_w4a16_g128_rk3576.rkllm` | 1,125,915,612 | `e3ec6b45e9e888063a6ee56d79ba4474c4416ec8db17f0ca7026a624e1c940d6` |
| `libsherpa-onnx-c-api.so`（v1.13.8 预编译） | 4,613,032 | `11378e1aea169aecb59a4d41ca8da6f5ee4f3614c34a4b908dd78de7646a518d` |
| `sherpa-onnx/c-api/c-api.h`（v1.13.8） | 167,657 | `2a1b95084be8fd1deb3228fcad2fd3f7f0258b64582f7402281ec174c7b7f4ce` |
| ASR `encoder-epoch-99-avg-1.int8.onnx` | 42,980,793 | `db6f51551762e40e549166fe041ea3e45464370b595e9ad23f06478ec3794fbb` |
| ASR `decoder-epoch-99-avg-1.int8.onnx` | 3,486,740 | `4b618d383af304cfae281dbf0a53e8bf442c2f0502256cd5694bd6567ebdd834` |
| ASR `joiner-epoch-99-avg-1.int8.onnx` | 3,228,485 | `bdda356d6f9b8c2d7cee9ee0e26075fa537490f7fd06520be408d287073667b9` |
| MeloTTS `encoder-zh.onnx` | 31,397,760 | `a2b0a5bc2789faef16b4bfc56ab4905364f8163a59f2db3d071b4a14792bfee5` |
| MeloTTS `decoder-zh.rknn` | 78,360,973 | `071d8dd5e67803eeb54c4d75b875c2ed11900a28c398f281fdc613af43e8a537` |
| MeloTTS `lexicon.txt` | 6,837,671 | `7236884b02435ac5d10cf69b4be40a61b45aa676b5300f0e412f185748fee528` |
| MeloTTS `tokens.txt` | 655 | `d18664a7e12bd7ea1022ddaf951e534e136815016c5a809d6b64156bffb4369d` |
| MeloTTS `g-zh_mix_en.bin` | 1,024 | `c70d897674847882bd35e780aee696ddaff8d04d5c57e4f9cf37611b6821879f` |
| MeloTTS `libonnxruntime.so.1.22.1` | 17,708,792 | `7b28635814a0810407791b3e26aff9dccf1b4563210baf3b71133231a35c5f2e` |
| MeloTTS `librknnrt.so` | 7,726,232 | `d31fc19c85b85f6091b2bd0f6af9d962d5264a4e410bfb536402ec92bac738e8` |

ASR 三个 int8 onnx 的大小与 SHA256 与 `artifacts/upstream-baseline/upstream-baseline.md`
第 2 节记录的上游基线完全一致；Qwen3.5 模型哈希与 `artifacts/llm-integration/`
记录一致。

## 4. 恢复过程中的网络约束（可复现）

本机与板端对 GitHub 的可达性是**分端点**的，直接 `curl https://github.com/...`
会超时，但下列端点可用：

| 端点 | 用途 | 结果 |
|---|---|---|
| `raw.githubusercontent.com` | 单文件（头文件） | 可用 |
| `codeload.github.com` | 仓库归档 zip | 可用（**必须直连 codeload**，走 github.com 跳转会超时） |
| `api.github.com` | release 元数据与资产下载 | 可用 |
| `api.github.com/repos/<r>/releases/assets/<id>` + `Accept: application/octet-stream` | release 资产 | 可用（本次 sherpa 预编译件 28,091,778 B / 69 s） |
| `github.com`（含 release 页面跳转） | 网页/跳转 | 实测超时 |
| `hf-mirror.com` | 国内镜像 | 板端超时，不可用 |

具体影响与绕行：

1. **sherpa-onnx 源码构建**：`cmake` 配置阶段会 FetchContent 下载
   `kaldi-native-fbank`、`kissfft` 等；`kissfft` 的 `github.com/.../archive/...zip`
   跳转超时，配置挂起。
   **绕行**：从 `codeload.github.com/mborgerding/kissfft/zip/<sha>` 直连下载，
   得到与上游 `cmake/kissfft.cmake` 期望值完全一致的 SHA256
   `497103e664168ebe39580b757adbe616f6cf85a16572af581ca7bc42d0ab13fd`，
   放到 `cmake` 的 `possible_file_locations` 之一（`/tmp/kissfft-<sha>.zip`）。
2. **最终采用的 sherpa-onnx**：为避免在 4 GB 板卡上长时间编译以及继续依赖
   分端点网络，改用 v1.13.8 官方 `linux-aarch64-shared-cpu` 预编译件
   （含自带 ONNX Runtime），并单独取 **同 tag** 的 `c-api.h`，保证头/库 ABI 一致。
3. **链接期约束**：`libsherpa-onnx-c-api.so` 的 `DT_NEEDED` 含
   `libonnxruntime.so`；只把该库放在 `build/_deps/onnxruntime-src/lib` 会导致
   `undefined reference to OrtGetApiBase@VERS_1.28.2`。**必须同时放在
   `build/lib/`（与 `libsherpa-onnx-c-api.so` 同目录）**，链接器才会解析传递依赖。
   两个目录都保留，`start.sh` 的 `LD_LIBRARY_PATH` 检查因此仍然成立。

## 5. 与上一代基线的差异

| 项 | 上一代基线 | 本轮 |
|---|---|---|
| LLM 模型 | DeepSeek-R1-Distill-Qwen-1.5B | Qwen3.5-0.8B W4A16 G128 |
| LLM Runtime | RKLLM 1.2.0 | RKLLM 1.3.0 |
| TTS 后端 | SummerTTS（`single_speaker_fast.bin`） | **MeloTTS**（ONNX Runtime CPU 编码器 + RKNN NPU 解码器） |
| TTS 模型来源 | 作者网盘（不入库） | 板端既有 MeloTTS 导出件（`decoder-zh.rknn`） |
| sherpa-onnx | 板端源码编译（ONNX Runtime 1.17.1） | v1.13.8 官方预编译件（自带 ONNX Runtime） |
| ASR 模型 | 同名 int8 zipformer | 同名 int8 zipformer（哈希逐项一致） |

TTS 后端替换的原因：SummerTTS 的 `single_speaker_fast.bin` 只在该项目作者的
百度网盘中分发，板端与开发机均已不存在该文件；MeloTTS 的模型与运行时在板端
齐全，且参考工程 nexweave 已用同一套模型跑通真实推理
（`~/workspace/nexweave-board/run_melotts_hardware_test.sh`）。因此本轮把
MeloTTS 作为板端默认 TTS 真实后端接入 SlotNexus 的 `ITtsBackend` 契约，
SummerTTS 后端保留兼容但默认不构建。
