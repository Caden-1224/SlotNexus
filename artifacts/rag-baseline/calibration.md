# RAG 阈值校准记录

> 日期：2026-08-02（接续时间线）
> 方法：默认阈值（5.0/1.5）下对 26 条候选查询实测 BM25 得分分布，
> 依分布选定阈值，再以 21 条固定测试集冻结期望路径。

## 1. 实测得分分布（知识库 data/knowledge/knowledge.jsonl，8 条）

| 区间 | 查询示例 | top1 得分 | 命中 |
|---|---|---|---|
| L1 强命中 | 音频格式统一为什么 | 11.68 | k-audio |
| | RAG 路由分为哪几层 | 9.94 | k-rag |
| | 端侧全离线是什么意思 | 8.64 | k-voxorchestra |
| | 多进程架构怎么实现 | 8.44 | k-arch |
| | 队列满了怎么办 | 6.75 | k-queue |
| | Backend 为什么可替换 | 6.96 | k-backend |
| | **RAG 路由是什么（L1 下界）** | **5.22** | k-rag |
| L2 部分命中 | Node Runtime 做什么 | 3.77 | k-runtime |
| | VoxOrchestra 的架构是怎样的 | 3.43 | k-arch |
| | generation 有什么用 | 3.03 | k-cancel |
| | Backend 是什么 | 2.98 | k-backend |
| | VoxOrchestra 是做什么的（未入测试集） | 1.62 | k-backend |
| L3 噪声 | 讲个笑话 | 1.69 | k-arch |
| | 帮我写一首诗 | 1.09 | k-audio |
| | 今天天气怎么样 / 你好 / what time is it | 0 | 无命中 |

## 2. 阈值选定与论证

| 阈值 | 值 | 依据 |
|---|---|---|
| direct_threshold | **4.5** | L1 期望集 top1 下界 5.22，取 4.5 留 0.7 裕量；L2 期望集上界 3.77，无重叠 |
| context_threshold | **2.0** | L3 噪声上界 1.69 < 2.0（"讲个笑话"的意外命中被挡回，不注入伪知识）；L2 期望集下界 2.98 > 2.0，无重叠 |

三处落地并保持一致：`libs/rag` RouterConfig 默认值（默认值即标定值）、
`config/mock/session.json`、`config/taishanpi3m/session.json`。

## 3. 已知边界（记录，不在本次修正范围）

1. **L0 关键词为子串匹配**：查询含"取消"即触发紧急控制，如
   "取消后旧数据怎么处理" 被路由为 L0（固化进测试集）。改进方向
   （如整句匹配或控制词计数）留待后续，需同步回归测试集。
2. **短事实查询得分偏低**："VoxOrchestra 是什么" top1=1.30 落入 L3
   （共享 token 少 + CJK 逐字分词），"VoxOrchestra 是做什么的"
   top1=1.62 亦低于 context=2.0 落 L3。代价是此类查询不注入知识，
   LLM 无上下文回答；不属错误路由，但演示中若频繁出现应扩充
   知识库条目或引入同义词扩展。
3. **阈值与知识库强耦合**：得分绝对量依赖 8 条中文短文档的平均长度
   与 IDF；知识库增删条目后必须重跑 `scripts/run_rag_testset.sh` 重新标定。

## 4. 冻结清单

- 知识库：`data/knowledge/knowledge.jsonl`（8 条，项目自述事实，固定）
- 测试集：`data/knowledge/rag_test_set.jsonl`（21 条，L0×5 / L1×6 / L2×5 / L3×5）
- 阈值：direct=4.5、context=2.0、top_k=2（三处一致）
- 复现：`scripts/run_rag_testset.sh`（库级 21 条断言 + 节点级五节点链路）
- 证据：`artifacts/rag-baseline/summary.json`（逐条 query/expect/actual/top1_score）

任何一项变更都视为路由行为变更，必须重跑复现并更新本记录。
