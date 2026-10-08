# 后台记忆整理（Daydream）

2026-10-08 修订。本文替代此前“空闲小憩、被聊天打断、整次回滚”的设计。

## 职责与唯一入口

`MemoryConsolidationService` 负责海马区印象的模型分类、去重、合并和长期巩固。
AIBrain 仅负责触发和生命周期；启动、定时、积压检查、右键“整理记忆”及睡眠触发都复用它。
对话与整理可同时请求模型，整理自身每次最多一个模型请求。模型分析不持有数据库事务。

睡眠协调器的新任务只提交日记，取消日记不会撤销已完成的记忆整理。
旧版已决定 Commit 的双参与者睡眠事务仍按原记录恢复 memory 与 private_psyche；
只保留旧结果的 finalize/abort，不保留旧的模型分析流程。

## 新鲜度与持久生命周期

`freshness = max(0, 1 - max(0, now - lastMentionedAt) / 3h)`。
真实提及会刷新 `lastMentionedAt`；缺失的旧数据回退到 `createdAt`。
查询、访问强化、整理和重启均不刷新提及时间，离线时间也参与衰减。

新鲜度仅影响候选评分，降到零不会删除未巩固信息。海马区持久数据没有 200 条硬截断；
200 是召回工作集窗口，积压必须留在 SQLite 中等待整理。显式到期、删除和敏感信息仍遵循资格过滤。

普通自述经提取规则进入海马区。显式“记住/忘记”和明确偏好继续使用 MemoryPolicy，
可直接影响长期记忆；整理必须同时校验源记录及模型拟更新的长期目标。
assistant 回复不进入用户印象，TaskShadow 不交给模型整理。

## 快照、分批与原子提交

1. 在现有 `ChatSideEffectQueue` 的数据库线程读取已提交数据，固定最多 32 个源 ID。
   批次选择保留最老记录的机会；源记录依然 Active、可召回。
2. 将本轮源 ID 的有界占用写入既有 `sleep_staged_change` 表，不新增另一套作业数据库。
   占用默认 10 分钟，崩溃后自动变为可重试。每批最多 8 条。
3. 模型看到不可变内容、创建时间和最近提及时间。相对日期必须按原始记录解释。
4. 模型结果先写入同一暂存表，再逐批用短事务应用。事务内重新从 SQLite 读取并检查源和目标。
5. 长期结果、源状态、DerivedFrom、图关系、索引 outbox 和 Finalized 标记同事务提交。
   一批失败不撤销其他已提交批次；本批相关源保留，等待重试。

| 决策 | 源记录 | 长期结果 |
|---|---|---|
| create / keep_both | Consolidated，保存 consolidated_into | 新建 |
| update | Consolidated，保存 consolidated_into | 更新校验通过的目标 |
| discard | Archived | 无 |
| preserve / 失败 / 冲突 | Active | 不强行落入长期区 |

源记录不物理删除。整理期间新写入的 ID 不在快照中，留给下轮。
后台分析与聊天持久化复用同一个写入队列；没有第二个整理写线程。
定时任务投影、显式工具写入等保留既有接口，但整行更新必须读数据库当前值并校验、合并。

## 并发更新规则

冲突版本比较涵盖内容、类型、状态、隐私、有效期、标签、证据等语义字段。
标签和证据按集合比较，不把 SQLite 返回顺序当作修改。
`updatedAt`、访问次数、强度、提及时间/次数和会话归属不单独使模型决策失效；
提交使用当前记录中的这些信息，不能回写模型开始时的整行快照。

聊天队列的更新按 before/after 增量合并当前访问和提及信息。删除、隐私或内容冲突拒绝旧写入。
用户的排队遗忘请求仍优先生效：即使源刚被巩固，也删除源及其对应的巩固结果。
若提及到达时源已经 Consolidated，且源的内容与资格未另行改变，新提及形成新的待整理印象，
原始源保持 Consolidated，不被恢复成 Active。相关索引任务跟随实际写入的 ID。

## 恢复与重试

- 自动整理结果使用 `session_id=maintenance`、`target_type=daydream_maintenance`。
  启动时重放 Prepared 结果；Finalized 标记保证幂等。
- 每条源的占用/冷却复用同表的 `maintenance-retry` / `daydream_retry` 记录。
  不确定或失败后默认冷却 30 分钟；语义证据改变可重新评估。
- 旧睡眠暂存结果不由自动整理直接重放，必须先有旧协调器的 Commit 决定。
- 停机、关闭 AI 或切换运行时会使取消 token 失效。迟到的模型结果不再提交；
  已完成的小批次保留，已持久暂存结果下次可恢复。
- 用户交流、键鼠活动和提醒临近不会取消整理，没有“打断后额外退避”。

## 调度与配置

启动后安排后台检查；默认每 30 秒检查一次，距上次整理至少 15 分钟，每小时最多 3 次。
积压达到 64 条时，最短间隔缩至 1 分钟，仍受小时限额约束。手动入口可主动发起一次，
同一时刻已有整理则复用“正在整理”的状态，不创建并行整理任务。

有效配置位于当前 AI profile 的 `daydream`：

| 配置 | 默认 | 范围/含义 |
|---|---|---|
| enabled | true | 自动整理和普通印象收集开关 |
| minIntervalMs | 900000 | 普通触发最短间隔 |
| hourlyLimit | 3 | 自动触发限额 |
| tickIntervalMs | 30000 | 检查周期 |
| sessionLimit | 32 | 1–32 条冻结源 |
| batchLimit | 8 | 1–8 条，且不超过 sessionLimit |
| relatedMemoryLimit | 8 | 0–8 条长期候选 |

旧的 idleThresholdSec、dueSoonThresholdMs、interruptionBackoffMs、inboxLimit 不再使用，
旧配置包含它们时会被忽略。睡眠配置中的 maxItemsPerSession、hippocampusBacklogThreshold、
relaxedIdleSeconds 也已移除，避免重复控制记忆整理。模型仍经 ModelRole::Daydream 路由。

## 用户体验与验证

界面入口为“整理记忆”，提示可以继续聊天。取消只表达技术生命周期，
不再产生悲伤情绪、“小憩被打断”记忆或相关人格事件。

回归覆盖真实 AIBrain 对话与整理并发、逐批进展、迟到取消、持久结果重放、
新旧记忆隔离、提及/访问合并、资格冲突、冷却跨重启以及旧睡眠事务恢复。
macOS 构建保持 ONNX Runtime 关闭；Windows 的真实模型链路仍需在 Windows 环境验收。
