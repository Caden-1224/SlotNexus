# 模型获取说明

本目录不存放任何模型文件（`.rkllm` / `.onnx` / `.bin` 等），只记录获取来源与版本链。

| 模型 | 用途 | 版本链（镜像/驱动/Runtime/模型） | 来源 | 校验 |
|---|---|---|---|---|
| `sherpa-zipformer-bilingual-zh-en-2023-02-16/`（int8） | sherpa-onnx streaming zipformer 中英双语 ASR（ASR 后端，板端） | Ubuntu 24.04 / onnxruntime 1.17.1（aarch64）/ sherpa-onnx 板端源码编译 | sherpa-onnx 官方模型，部署时注入 |  |
| `Qwen3.5-0.8B_w4a16_g128_rk3576.rkllm` | RKLLM 大模型生成（LLM 后端，板端） | Ubuntu 24.04 / RKNPU driver 0.9.8（内核 6.1.99）/ librkllmrt.so v1.3.0 / 模型 W4A16_G128（RK3576，max_context_limit 4096） | rknn-llm model zoo，部署时注入 |  |

当前板端 TTS 为 MeloTTS（`encoder-zh.onnx` + `decoder-zh.rknn` + lexicon/tokens/g 向量），
旧 SummerTTS 模型与代码已退出当前构建与部署路径。

规则：

- 每个模型必须与其 Runtime、驱动、镜像版本配套记录。
- 模型体积大，一律不进入仓库，由部署路径注入。
