# 协议与标识符语义

## 外部帧

客户端与 Edge Gateway 使用 UTF-8 JSON，每条消息以换行结束（NDJSON）。
最大序列化尺寸为 1 MiB。协议版本当前为 `1`；未知版本、未知类型、缺少
`type`、字段类型错误或超长消息均返回结构化错误，不应终止服务进程。

## MessageEnvelope

| 字段 | 类型 | 语义 |
|---|---|---|
| `version` | integer | 协议版本，当前必须为 1 |
| `work_id` | string | 一次 setup 创建的任务生命周期，由 Unit Manager 分配 |
| `request_id` | string | 一次控制调用或推理请求，用于响应关联和日志追踪 |
| `session_id` | string | 多轮会话关联；无多轮上下文时允许为空 |
| `type` | string | setup / inference / cancel / taskinfo / exit / event / ack / error |
| `index` | integer | 数据流内序号；非流式消息缺省为 -1 |
| `timestamp_ms` | integer | 发送方 UTC 毫秒时间；未设置时为 -1 |
| `payload` | object | 动作或事件负载 |
| `finish` | boolean | 当前流是否结束 |
| `error` | object | 无错误时 `{}`；错误时包含 `code` 和 `message` |

## 标识符不变量

| 标识符 | 创建者 | 生命周期 | 不变量 |
|---|---|---|---|
| `work_id` | Unit Manager | setup 到成功 exit | 后续动作必须携带；未知或已释放 ID 返回 not_exist |
| `request_id` | 客户端 | 单次动作 | 响应和数据事件必须原样回显，不能跨请求复用在途 ID |
| `session_id` | 客户端 | 多轮会话 | 只关联上下文，不替代 work_id 或 request_id |
| `generation` | Session | 每次新推理/取消后递增 | 只接受当前代的 token 和 PCM，旧代直接丢弃 |

## 控制动作

| 动作 | 必需内容 | 成功响应 | 关键语义 |
|---|---|---|---|
| `setup` | `request_id` | `ack` + 新 `work_id` | 加载配置和模型；失败时不占用路由名额 |
| `inference` | `work_id`, `request_id`, `payload` | `ack` + 结果摘要 | 同一 work_id 单流，在途时返回 busy |
| `cancel` | `work_id`, `request_id` | `ack` | Ready 状态幂等；Busy 状态为协作式取消 |
| `taskinfo` | `work_id`, `request_id` | `ack` + state/in_flight/计数 | 只读快照 |
| `exit` | `work_id`, `request_id` | `ack` | 释放任务与路由；重复 exit 返回 not_exist |

固定 WAV 请求示例：

```json
{"version":1,"type":"inference","work_id":"w-0","request_id":"r-1","payload":{"mode":"wav","wav":"demo_zh.wav"}}
```

## 数据面事件

数据面同样使用 `MessageEnvelope`，`type=event`，主题前缀为
`<work_id>/<request_id>/`。`payload.kind` 为 `partial`、`final`、`token`、
`done` 或 `pcm`。文本事件使用 `payload.text`；PCM 使用 base64 编码，音频
格式为 16 kHz、单声道、16-bit PCM，常规帧为 320 个采样。

## 错误码

协议解析错误码依次为：0 ok、1 invalid_json、2 unknown_version、
3 invalid_type、4 missing_field、5 oversized。任务运行时错误码依次为：
0 ok、1 not_exist、2 bad_state、3 busy、4 timeout、5 cancelled、6 capacity。
两组编号属于不同层，调用方应同时检查响应 `type`、`error.code` 和
`error.message`，不能只按裸数字推断来源。传输层不可达使用负值错误码。
