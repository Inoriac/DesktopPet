# 设计：类人激活式记忆召回

## 背景与目标

当前 `MemoryRetriever` 在聊天热路径中遍历全部持久记忆并逐条打分；用户请求的偏好类型又覆盖大多数长期记忆，因此候选过滤较宽。Embedding 生产组装尚未接入，现有 SQLite 向量搜索仍是全量余弦计算。记忆关系图只在直接候选排序后扩展前三个节点的一跳邻居，标签共现图尚未参与召回；两张图没有成为定位和联想记忆的主要路径。

本次设计将召回改为类人激活式模型：近期和当前高激活记忆可以直接进入意识，长期记忆通过 HNSW 获得少量语义种子，再由标签图和记忆关系图传播联想，最后依据访问历史、时间、情绪、性格和当前线索选出有限记忆。聊天热路径不得全量扫描持久记忆，不调用大模型，召回成本应主要由固定候选预算决定，而不是随记忆总量线性增长。

本次不改动渲染系统，不追求完整复刻认知科学模型，也不让 HNSW 或图谱取代 SQLite 的权威数据地位。大模型只在记忆写入和 Daydream 阶段整理少量复杂关系，不参与在线召回排序。

## 方案

### 1. 总体架构

召回采用“短期激活 + 多路种子 + 图谱传播 + 统一精排”的固定流水线：

```text
当前会话
  -> 本地线索提取
  -> 近期激活池 / HNSW / 关键词与标签索引
  -> 合并、过滤、初始激活计算
  -> 标签图和记忆关系图的有预算传播
  -> ACT-R 启发式精排与人格化探索
  -> 最多 8 条、约 800 tokens 的相关记忆
  -> Runtime Context
```

新增 `MemoryCueExtractor`、`ActiveMemoryPool`、`HnswEmbeddingIndex`、`AssociativeActivationEngine` 和 `MemoryIndexWorker` 五个边界清晰的组件。`MemoryRetriever` 保留统一入口，但改为编排这些组件，不再自行扫描 `MemoryStore::all()`。召回返回每条记忆的来源、分数组成、传播路径和是否为探索候选，供测试、观测和 Prompt 格式化使用。

### 2. 记忆生命周期与索引边界

`Working`、`ShortTerm` 和处于 `Hippocampus` 分区的记忆不进入 HNSW，由 `WorkingMemoryCache` 与 `ActiveMemoryPool` 负责近期召回。Daydream 将其巩固为 Active 的长期记忆后，才计算 Embedding、写入 SQLite 向量表并增量插入 HNSW。这样短命、重复和尚未整理的经历不会污染长期语义索引。

HNSW 只包含 Active、非 Sensitive 的长期记忆。长期记忆内容发生语义变化时执行“旧向量标记删除 + 新向量插入”；状态变为 Archived、Superseded、Deleted、Expired 或 Sensitive 时标记删除。恢复为可召回状态时重新插入。SQLite 中的 `memory_items` 和 `memory_embeddings` 始终是权威数据，HNSW 是可丢弃、可重建的派生索引。新巩固或更新的长期记忆在索引任务完成前暂留 `ActiveMemoryPool`，避免 SQLite 已提交但 HNSW 尚未可见时出现召回空窗。

### 3. HNSW 实现与持久化

使用 `hnswlib` 新增 `HnswEmbeddingIndex`，实现现有 `EmbeddingIndex` 接口。默认距离为 cosine，初始参数为 `M=16`、`efConstruction=200`、`efSearch=64`；查询默认返回 32 个语义候选。参数进入配置并写入索引元数据，后续通过基准测试调整。

索引文件使用 `memory_hnsw_<model-hash>.bin`，旁路元数据保存模型名称与版本、内容哈希规则、向量维度、距离类型、HNSW 参数、索引 generation 和构建时间。SQLite 新增稳定的数值 label 与 `memoryId` 映射，避免把字符串哈希直接当 label 产生碰撞。模型、维度、建图参数 `M/efConstruction`、距离类型或 generation 不匹配，文件损坏，或索引计数与权威表不一致时，从 SQLite 向量表后台重建；`efSearch` 是运行时查询参数，变化不触发重建。

索引写操作由单个 `MemoryIndexWorker` 串行执行，查询使用读锁或只读快照，禁止 GUI 线程执行模型推理、索引重建和批量向量写入。删除墓碑比例达到 20% 时，由空闲期或 Sleep Cycle 重建索引，消除长期增删改造成的导航质量下降。

### 4. 一致性与故障恢复

SQLite 新增 `memory_index_jobs` 作为事务型 outbox，记录 `memory_id`、操作类型、模型版本、内容哈希、状态、尝试次数和创建时间。长期记忆变更与索引任务在同一个 SQLite 事务中提交；事务完成后，`MemoryIndexWorker` 幂等消费 `upsert`、`delete` 和 `rebuild` 任务。对于 upsert，Worker 校验内容哈希、生成向量并更新 `memory_embeddings`，随后更新内存中的 HNSW。

HNSW 文件无法参与 SQLite 事务，因此 Worker 合并一批任务后先将完整索引写入临时文件并原子替换正式文件，再更新索引 generation，最后在 SQLite 事务中标记这一批任务完成。程序在任一步异常退出都可以安全重放；同一 `memoryId + model + contentHash` 的重复 upsert 不产生重复有效节点。索引不可用或正在重建时，召回继续使用近期激活池和关键词/标签索引，不回退到聊天热路径的全量扫描。

### 5. 本地线索提取与多路种子

`MemoryCueExtractor` 只做本地计算：规范化用户文本，提取拉丁词元、中文二元/三元片段、已知标签和实体，并读取当前会话主题、未完成目标、情绪快照及 `PersonaProjection::behaviorParameters`。查询文本只计算一次 Embedding，然后交给 HNSW。在线召回禁止为线索提取调用大模型。

关键词和标签使用增量维护的倒排索引；其中标签继续落在现有 `memory_tags`，中文短语索引是可重建的派生结构。默认候选预算为：近期激活池最多 12 条、HNSW 最多 32 条、关键词/标签最多 12 条；去重和隐私/状态过滤后保留最多 16 个初始种子。某一路不足时不强行填满，也不通过扩大到全库补齐。

### 6. 激活模型与访问强化

最终排序采用 ACT-R 启发式而非完整 ACT-R 仿真。对候选记忆 `i` 计算：

```text
A_i = B_i + C_i + R_i + E_i + G_i

B_i = 1.0 * strength
    + 0.8 * importance
    + ln(1 + sum((1 + accessAgeHours)^-0.5))

C_i = 2.5 * semanticCue + 1.2 * lexicalOrTagCue
R_i = 1.5 * runtimeActivation
E_i = 0.6 * emotionMatch * currentIntensity * memoryEmotionConfidence
G_i = graphPropagation
```

所有 cue 值归一化到 `[0,1]`；cosine 相似度默认从 0.55 开始线性映射到 `[0,1]`，低于阈值不作为语义种子。访问历史只为最多 64 个候选批量读取最近 32 次记录，并以现有 `strength`、`importance` 和累计 `accessCount` 吸收更早历史。`memory_access_log` 增加 `(memory_id, created_at)` 索引。

只有最终进入 Prompt 的记忆才记录一次访问并获得强化；同一轮对话对同一记忆最多强化一次。取消当前“所有直接候选都强化”的行为，避免召回器自身形成不可控的赢家循环。访问日志写入异步持久化队列，不阻塞网络请求派发。

### 7. 运行时激活与离线恢复

`ActiveMemoryPool` 保存当前“脑海中”的少量记忆及其激活来源。运行期激活按来源使用不同半衰期：会话主题 60 分钟、一次联想 30 分钟、近期经历 6 小时、情绪激活 6 小时并按情绪强度缩放、未完成目标 24 小时且在目标仍有效时刷新。默认低于 `0.05` 时移出活跃池。

SQLite 保存最多 50 条激活快照，字段包括 `memoryId`、保存时激活度、来源、`lastActivatedAt` 和上下文标识。每轮交互结束后异步覆盖快照。重启时使用墙上时钟计算：

```text
restoredActivation = savedActivation * 2^(-offlineDuration / sourceHalfLife)
```

负时间差按 0 处理，异常大的时间差直接使瞬时激活失效。短时间重启可延续当时思路，长时间离线后瞬时激活自然消散；长期影响仍由强度、访问记录和最近访问时间保留。

### 8. 混合建图与图谱维护

关系图采用混合建图。`DerivedFrom`、`Supersedes`、`CreatedTask` 等具有明确来源的边由代码直接建立；HNSW 为新长期记忆提供相似邻居候选，结合标签建立或强化 `Related`；同一事件中的记忆建立 `MentionedWith`；Daydream 只对候选集合中的 `TopicOf`、`ConflictsWith` 和语义歧义关系做少量模型判断。

模型只能提交包含既有节点、枚举关系、置信度和证据的关系提案，不能自由创建节点或绕过隐私与状态校验。每个 Daydream 批次最多让模型判断 8 对候选。关系表增加 `provenance`、`support_count`、`updated_at` 和 `last_reinforced_at`；规则、工具、Embedding、共现和模型来源必须可区分。每个节点最多保留 32 条普通联想边，结构性边不受该上限影响。

Daydream 清理悬空边、合并重复边，并对弱 `Related`、`MentionedWith` 边做衰减；结构性的 `DerivedFrom`、`Supersedes`、`CreatedTask` 和确认后的 `ConflictsWith` 不按普通共现衰减。新 Daydream 巩固路径必须同时维护记忆关系图和标签共现图，不能只记录标签边。

### 9. 图谱激活传播

传播最多两跳，并同时受候选总量 64 和最低传播增量 `0.08` 限制。使用优先队列维护传播前沿，但不是每次都机械选择最高激活节点。一次传播增量为：

```text
delta = sigmoid(sourceActivation)
      * edge.weight
      * edge.confidence
      * relationTypeFactor
      * 0.55^hop
      / sqrt(max(1, traversableDegree))
```

默认关系系数为：`DerivedFrom=1.0`、`CreatedTask=0.9`、`ConflictsWith=0.8`、`TopicOf=0.75`、`Related=0.65`、`MentionedWith=0.4`。`Supersedes` 使用定向规则：旧记忆命中时将激活完整转向新记忆，新记忆不因该关系把已取代内容带回。冲突边成对召回并显式标记，不把冲突内容当成普通事实合并。

标签共现图先从明确标签扩展最多 4 个高权重邻接标签，再通过倒排索引产生记忆种子；记忆关系图随后传播具体经历。传播记录完整路径并防止环路，同一节点从多条路径获得的增量累加但封顶为 1。

### 10. 性格、情绪与受控随机联想

传播采用平均约 80% 的稳定利用和 20% 的人格化探索。稳定分支选择当前综合激活最高的前沿；探索分支使用 Softmax 加权抽样。基础探索概率为 `clamp(0.10 + 0.20 * openness, 0.10, 0.30)`，默认人格约为 22%；温度为 `0.15 + 0.50 * openness`。

`openness` 提高新奇联想，`sociability` 轻度提高 `Relationship` 类型记忆以及 `MentionedWith`、`Related` 边，`initiative` 轻度提高未完成目标与任务相关边，单项偏置不超过 15%。当前情绪提高情绪一致记忆的唤起概率，但不降低事实置信度。最终最多允许 1 条探索记忆进入 Top 8，其余位置按确定性激活排序。

隐私、状态、关系合法性和任务真实性过滤始终先于随机选择，随机性不能绕过安全约束。生产使用会话级随机源；测试注入固定随机种子，保证结果可重放。

### 11. 输出、预算与可解释性

完整候选池最多 64 条，最终输出同时满足“最多 8 条”和“约 800 tokens”两个限制；非用户主动触发延续当前较小预算，最多输出 4 条。每条结果保存 `sourceChannels`、各激活分量、图谱路径、关系类型和 `exploratory` 标记。默认 Prompt 仍只注入紧凑摘要、类型、置信度和必要的冲突提示，不暴露内部数值或原始推理过程。

调试日志只记录 memoryId、分数分量、候选来源和耗时，不记录 Sensitive 内容。观测指标至少包括线索提取、Embedding、HNSW、图谱传播和精排耗时，各来源候选数量、HNSW 访问量、探索候选命中率及最终强化数量。

### 12. 迁移与降级

数据库迁移先增加索引任务、稳定 label、关系元数据、激活快照和访问日志索引。随后由后台任务分批为现有 Active 长期记忆生成向量并构建 HNSW，不能阻塞启动。迁移期间新召回以近期激活和关键词/标签索引为主；HNSW 覆盖率达到 95% 并通过校验后参与正式结果。

上线前保留旧全量召回作为 shadow 基准，只记录新旧 Top-K 差异，不向 Prompt 注入两份结果。验收完成后，旧扫描仅在测试工具和离线诊断中使用。Embedding Provider 不可用、HNSW 损坏或重建时，系统降级到近期激活、关键词和标签召回，并报告索引健康状态，不在聊天热路径恢复全量扫描。

### 13. 验证策略与验收标准

单元测试覆盖线索提取、不同来源半衰期、离线恢复、ACT-R 基础激活、单轮去重强化、关系方向、两跳预算、环路、人格化概率分布、固定随机种子、隐私过滤和冲突成对召回。索引测试覆盖任意及按时间顺序插入、增量更新、墓碑删除、崩溃任务重放、模型切换、文件损坏和 SQLite 重建。

使用精确全量余弦 Top-K 作为离线真值，在至少 1 万条 512 维向量上要求 HNSW `Recall@32 >= 0.95`。参考桌面环境中，预热后的 HNSW 查询、候选合并、图谱传播和精排 P95 不超过 30ms；包含查询向量生成在内的端到端本地召回 P95 不超过 100ms，GUI 线程不执行其中任何重任务。真实对话评测还需覆盖近期记忆、强印象记忆、跨措辞语义、两跳联想、矛盾记忆、长时间重启和人格化但不离题的探索。

代码验收要求聊天召回路径不再调用 `MemoryStore::all()`，结果规模不随长期记忆总量线性增长；任何进入 Prompt 的记忆均能解释其种子来源、激活组成和图谱路径。

## 澄清结论

- [2026-09-02] 召回采用符合类人表现的混合激活模型，近期记忆走独立激活池，Daydream 巩固后的长期记忆才进入 HNSW。
- [2026-09-02] 引入 HNSW 作为正式语义索引，使用 `hnswlib`；SQLite 保存权威记忆与向量，HNSW 文件作为可重建派生索引。
- [2026-09-02] 瞬时激活允许跨重启恢复，但必须按照离线时长和激活来源半衰期衰减；长期印象继续由强度和访问历史表达。
- [2026-09-02] 最终排序采用 ACT-R 启发式激活模型，不完整复刻 ACT-R；大模型不参与在线召回。
- [2026-09-02] 在线热路径只执行本地线索提取、索引查询、图谱传播和排序；模型只在写入与 Daydream 阶段判断少量复杂关系。
- [2026-09-02] 关系图采用确定性规则、Embedding 候选和模型复核相结合的混合建图，新 Daydream 路径必须同时维护记忆关系图与标签图。
- [2026-09-02] 图谱传播最多两跳，受激活阈值和 64 条候选预算约束，不进行无界遍历。
- [2026-09-02] 联想平均采用约 80% 稳定选择和 20% 性格、情绪控制的探索，最终 Top 8 最多保留一条探索记忆。
- [2026-09-02] 初始候选默认来自近期激活 12 条、HNSW 32 条、关键词/标签 12 条；最终最多 8 条且约 800 tokens，所有参数可配置并由评测校准。
