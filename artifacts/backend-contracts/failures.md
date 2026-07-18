# backend-contracts 失败记录

## 缺陷 1：asr_node 默认端口与 echo_node 冲突（已修复）

**现象**：fake_nodes_e2e_test 首次运行失败 —— 部分节点 "Address already in use"
启动失败；后续推理回复 request_id 错位（收到上一条请求的回复），最终
nlohmann 解析到非法字节崩溃。

**根因**：asr_node 复制 echo_node 模板时沿用了默认端口 `tcp://127.0.0.1:19200`，
与 echo_node 的默认端口完全相同。两个进程竞争绑定同一端口，先到者成功、
后到者退出；Manager 轮转路由把请求发到死端点后，回复流错位。

**修复**：固定端口约定并写入各节点用法注释：
echo 19200 / asr 19201 / rag 19202 / llm 19203 / tts 19204；
E2E 改用各节点默认端口（不再显式传 --listen）。

**回归**：修复后 fake_nodes_e2e_test 通过；全量 CTest 19/19。

## 缺陷 2：asr_node 适配器按 JSON 对象解析 payload（已修复，Step 2 阶段）

见 Step 2 核验材料：RuntimeNode 已用 ExtractText 提取 text 字段，适配器收到
纯文本；按 JSON 对象解析会回落为 1 帧。已改为直接 stoi(payload) 并注释约定。
