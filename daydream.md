# Daydream — 桌宠空闲记忆整理设计

> 从 `memory_improvement_plan.md` 拆出。基本记忆框架（Embedding 检索 / RRF 融合 / 自适应遗忘 / 分区迁移）先行落地，Daydream 作为第二阶段独立实现。本文为 Daydream 的完整设计。

> 2026-08-16 设计审计修订：全 session 原子性改为“快照 + 内存 staging + 最终短事务提交”。异步 LLM 等待期间不得持有 SQLite 写事务；用户交互产生的新 inbox 项也不属于当前快照。

---

## 一、定位与边界：Daydream ≠ 写日记

Daydream 是作者提出的桌宠**空闲时刻自主记忆整理**机制。它**不是写日记**：

- **写日记 = 录入**：把当天发生的事条目化记下来，输出是「更多的记忆条目」。
- **Daydream = 消化**：对已在 Working Memory / Hippocampus 待巩固区的碎片印象做**分类、去重、冲突合并、重要性再评估**，沉淀成关于用户/偏好/事实的结构化长期认知，输出是「更少但更结构化的长期记忆」，inbox 被清空。

不是"记录今天发生了什么"，而是"把白天碎片化的交互印象，琢磨成关于用户/偏好/事实的结构化长期认知"。桌宠可以在用户不交互的空闲时段"自己琢磨"。

| 维度 | 写日记（录入） | Daydream（消化） |
|------|--------------|----------------|
| 输入 | 当天原始事件流 | 已在 Hippocampus 待巩固区的碎片印象 |
| 动作 | 新增条目 | 分类/去重/合并/再评估 |
| 输出 | 更多的记忆条目 | 更少但更结构化的长期记忆（inbox 被清空） |
| 触发 | 主动记录 | 空闲异步 |
| LLM 角色 | 可无 | 核心（分类+冲突决策） |

## 二、现状校正

当前分支已有 Daydream 的全局空闲触发器、Hippocampus 快照、异步批量 LLM 决策、硬编码失败降级和最终短事务提交。检索路径已只读，不再同步巩固。运行开关、触发阈值、容量、批次和独立轻量模型均已配置化；标签共现图也已落 SQLite 并纳入最终短事务。尚需继续用真实模型输出做兼容性验证。

## 三、触发设计：大概率空闲 + 解耦 + 可中断

### 3.1「大概率空闲」复合判定

不靠单一信号，满足以下**全部**条件才视为空闲，触发一次 Daydream session：

- **系统级全局空闲 ≥ N₁ 分钟**——即"距离上次键鼠输入的时间"≥ N₁，复用现有 `get_user_idle_state`（core/ai/tools/environment_tools.cpp）能力，**不是 pet 窗口输入**。关键区分：只看 pet 交互会把"用户没理桌宠但正在全力工作"误判为空闲、在最该安静时触发，所以必须用全局空闲。该能力注释已写明"不读取屏幕、窗口标题或输入内容"，隐私面已收口。N₁ 初拟 5 min。
- `AIBrain::m_busy == false`（无进行中的 LLM 对话）
- 无待办 ScheduledTask 即将到期（`AgentScheduler` 距最近 due > N₂ min，初拟 10 min）
- 距上次 Daydream ≥ 间隔下限（初拟 15 min，防止连续触发）；若上次是被打断退出，额外退避 ≥ 10 min（防抖动：反复触发又打断）

任一条件不满足则不触发；触发后**运行期间持续监测**，一旦从空闲滑入非空闲（用户开始输入 / 新对话开始 / 待办即将到点）→ 触发**协作式中断**（见第五节）。监测 tick 频率初拟每 30s 复查一次 `get_user_idle_state` 的 idle_seconds 跳变为 active 即判滑入非空闲——不必毫秒轮询，也无需等当前 LLM 批跑完（中断可发生在批之间）。

### 3.2 平台降级（macOS 空闲缺口）

现有 `get_user_idle_state` 仅 Windows 实现（`GetLastInputInfo`），macOS/Linux 返回 `supported=false`。Daydream 触发依赖该能力：

- **macOS 须补实现**（`CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateCombinedSessionState, ...)`，只读时长、不读内容、无需辅助功能权限），与 Windows 对齐。
- Linux 可后置（X11 `XScreenSaverQueryInfo` 或 D-Bus）。
- 平台不支持 / 触发失败时**默认不触发**（宁可不 daydream 也不误触发抢资源），退化为仅 `m_busy=false` + 距上次对话 ≥ dwell + 无待办的可选降级判定；该降级默认关闭，需配置显式开启。

### 3.3 从检索路径解耦

删除 `retrieveMemoryHints`（ai_brain_router.cpp:272）里的同步 `cleanup()` 调用，检索路径只读不整理；整理由 Daydream 异步执行，避免把 LLM 巩固延迟压到用户提问。

### 3.4 节流

单 session 总量上限为 32 条待巩固印象，分批每批 ≤8。触发时取得带 revision 的快照；处理期间不锁待巩固区，新写入项留给下次 session。最终提交前若快照内任一源条目已变化，则拒绝整次提交并在下轮重算。

每小时 Daydream 上限 ≤3 次（防一直空想）。

## 四、整理流程（对齐 hebb-mind ConsolidationAgent）

```
空闲触发
  → 收集 Hippocampus / 到期 Working 中待巩固记忆（按时间/turn 排序）
  → 召回相关历史记忆（排除 Hippocampus，供冲突对比）
  → LLM 决策（批量）：
      1. 目标分区分类（Semantic/Episodic/Preference/Procedural）
      2. 冲突检测（与历史记忆对比）
      3. 标签提取（3-5 个有意义标签）
      4. 重要性评分（0-10）
  → 冲突解决：update（合并）/ keep_both（并存）/ discard（丢弃）
  → 写入目标分区，删除源记忆（清空 Hippocampus inbox）
```

### LLM Prompt 模板

```
你是桌宠的记忆整理模块（Daydream）。请把以下待巩固的碎片印象「消化」为结构化长期认知，
并与相关历史记忆做冲突对比。注意：你不是在写日记记录事件，而是在归类合并印象。

【待巩固印象】（一批）
内容：{content}
当前标签：{tags}
来源：{source}

【相关历史记忆】（供冲突检测）
{related_memories_json}

请对每条印象决策（输出 JSON 数组）：
{
  "target_partition": "Semantic|Episodic|Preference|Procedural",
  "action": "create|update|keep_both|discard",
  "merged_content": "合并后内容（仅 update）",
  "quality_score": 0-10,
  "new_tags": ["tag1", "tag2"],
  "reason": "简短理由"
}
```

### 会话级批量巩固

```cpp
void Daydream::consolidateBatch(const std::vector<Memory>& memories) {
    auto snapshot = takeSnapshot(memories, 32); // 不开写事务
    std::vector<Decision> staged;

    constexpr int CHUNK_SIZE = 8;
    for (size_t i = 0; i < snapshot.size(); i += CHUNK_SIZE) {
        if (cancelled()) return;            // staging 尚未落库，直接丢弃
        auto chunk = slice(snapshot, i, CHUNK_SIZE);
        staged += co_await callLLM(chunk);  // 网络等待期间 SQLite 可正常写
    }
    if (cancelled()) return;
    commitIfSnapshotCurrent(snapshot, staged); // 一个很短的本地事务
}
```

**Token 成本估算：** 每 8 条印象一次调用，单次约 1000-2000 token；空闲触发、低频、可配置开关。

## 五、中断、快照与事务原子性（核心需求）

作者要求：Daydream 运行中一旦滑入非空闲，**打断操作并保持触发前的用户可见记忆状态**，杜绝图残留和迟到回调写入。设计审计后，这映射为**不可变快照 + 内存 staging + generation 取消 + 最终短事务**，而不是跨异步网络调用长期持有 SQLite 写锁。

### 5.1 快照与最终短事务（全 session 原子语义）

触发时按创建时间取得最多 32 条 Active Hippocampus 项及 revision 快照。多个 LLM 批次只把决策暂存在内存，不修改数据库。全部批次完成、空闲条件仍成立且快照未变时，才开启一个短事务，将删源、写目标分区、更新历史记忆和关系图作为一个原子提交。

```cpp
auto snapshot = store.snapshotHippocampus(/*limit=*/32); // 无事务
auto generation = sessionGeneration;
for (auto chunk : chunks(snapshot, 8)) {
    auto decisions = co_await callLLM(chunk);
    if (generation != sessionGeneration) return;         // 丢弃 staging
    staged.append(validate(decisions));
}
if (!idleStillValid() || !snapshotStillCurrent(snapshot)) return;

db.transaction();                                        // 只包本地写入
if (applyAll(staged, db)) db.commit();
else db.rollback();
```

- **关系图无残留**：`memory_relations` 与 `tag_cooccurrences` 均复用 MemoryStore 的 SQLite 连接，并与记忆写入处于最终同一短事务。共现权重表示标签对被 create/update 巩固确认的事件次数。
- **新 inbox 不被误删**：快照建立后到达的新项不属于当前 session；下次再处理。
- **源项变化时拒绝提交**：若同 key 的再次提及更新了 mentionCount/content/revision，旧 LLM 决策已失效，整 session 不落库。
- **无批级可见状态**：批次只产生 staging 结果，不逐批提交，因此中断不会留下已完成批。

### 5.2 generation 协作式取消（防迟到回调残留）

LLM 调用走 `ChatService::requestAsync` 异步回调，强杀网络请求不现实。每个 session 捕获 generation；用户交互、待办临近、空闲结束或 AIBrain stop 都递增 generation 并清空 staging：

- **回调丢弃**：回调先比较 generation，不匹配则直接返回。
- **提交前复查**：最后一个批次后再次复查全局空闲、AIBrain busy、待办距离和快照 revision。
- **异常回滚**：只有最终本地 apply 已开启事务后发生写失败时才需要 `ROLLBACK`。

这使“用户可见状态全 session 原子”与“用户对话不等待远程 LLM/SQLite 长写锁”同时成立。

### 5.3 并发写行为

LLM 运行期间没有 Daydream 写事务，日常对话可以正常写入 Hippocampus 和长期记忆。最终 apply 在 Qt 所属线程执行，事务仅覆盖本地校验后的有限写操作。用户输入事件若恰好与最终 apply 同时到达，最多等待这段短本地提交，不等待网络请求。

## 六、Working Memory → Hippocampus 改造

Hippocampus 的持久输入必须是“关于用户的待判断印象”，不能是桌宠刚刚生成的 assistant reply，否则 Daydream 会把自己的措辞误当成用户事实，实质退化为写日记。当前入口规则：

- 显式“记住/忘记”继续走确定性的 MemoryPolicy，立即生效，不重复进入 inbox。
- 普通用户输入只有在包含自述信号、长度有界且未命中敏感信息规则时，才以 `Personal` ShortTerm impression 进入 inbox。
- 完全相同的自述按稳定 key 合并并增加 mentionCount，而不是创建重复行。
- inbox 设 200 条硬上限；达到上限后仍允许更新已存在的同 key 印象，但不继续无界增长。
- assistant response 不进入持久 Hippocampus；工具结果可留在易失 WorkingMemoryCache 供当前上下文使用。

| 维度 | 当前 WorkingMemoryCache | 改造后 Hippocampus |
|------|------------------------|-------------------|
| 定位 | TTL 过期的临时缓存 | 工作记忆收件箱 + 分类暂存 |
| 过期策略 | 固定 TTL（30/15/60/20 min，按 source 分流） | 基于 Daydream 巩固决策 + 容量上限 |
| 巩固触发 | 检索路径同步 cleanup，硬编码 mentionCount≥2 或 emotion≥0.7 | Daydream 异步 LLM 分类决策 |
| 容量管理 | importance 最低淘汰（trimToCapacity） | FIFO + 重要性优先保留 |
| 输出目标 | 固定写 Episodic（混合） | 明确分区（Semantic/Episodic/Preference/Procedural） |

ShortTerm/TaskShadow 存量分流：都先进 Hippocampus inbox，由 LLM 判定再分，不预先硬分流。

## 七、空输出与异常处理

| 场景 | 处理 |
|------|------|
| LLM 判定无价值（如纯闲聊）→ 返回 discard/空 | 删除源印象，清空 inbox（对齐 hebb `consolidation_drain_empty_sources`） |
| LLM 输出解析失败 | 保留源印象 → 下轮 Daydream 再处理 |
| LLM 调用超时 | 保留源印象 → 降级为原硬编码规则（mentionCount≥2 / emotion≥0.7） |
| **运行中滑入非空闲** | generation 失效 → 丢弃在途/迟到回调和内存 staging；此时尚未写库，无需等待或回滚 |
| 最终 apply 写失败 | 短事务 `ROLLBACK`，随后从 SQLite 重载内存镜像 |
| 快照源条目在 LLM 期间变化 | 拒绝整 session 提交，保留最新源条目供下轮重算 |

原硬编码巩固规则（`mentionCount≥2` / `emotion≥0.7`）并存到 Phase 3，作为 Daydream 失败降级兜底，验证稳定后删。

## 八、风险

| 风险 | 影响 | 应对 |
|------|------|------|
| Daydream LLM 巩固增加 token 消耗 | 成本上升 | 空闲低频触发（每批 ≤8 条），可配置开关 |
| 检索路径解耦后遗漏清理 | inbox 堆积 | Daydream 必须有兜底定时触发，不能只依赖空闲 |
| 空闲检测误判（如用户离开但进程未空闲） | 巩固抢资源/频繁打断 | 复合判定（全局空闲+m_busy+待办）+ 最小空闲时长 + session 总量上限 + 打断后退避 |
| macOS/Linux 无全局空闲实现 | 非平台无法触发 Daydream | macOS 补 `CGEventSource`（Daydream 前）；Linux 后置；不支持时默认不触发，可选降级判定 |
| Daydream 中断留下图/回调残留 | 半修改状态污染图谱、漏写 | LLM 阶段只 staging；generation 丢弃迟到回调；最终记忆与图写入同一短事务 |
| SQLite 长事务持锁 | 阻塞日常对话写入 | 禁止跨 LLM 持事务；最多 32 条快照，全部决策完成后才短事务 apply |
| 把 assistant reply 当用户记忆 | 形成自我引用和伪用户事实 | inbox 仅接收过滤后的用户自述 impression；显式记忆仍走确定性策略 |

## 九、已定决策

- **Daydream 输入口径**：用**系统级全局空闲**（`get_user_idle_state`，即日常使用输入，非 pet 窗口输入）；macOS 补 `CGEventSource`，Linux 后置，不支持时默认不触发。理由：只看 pet 输入会把"用户忙碌工作"误判为空闲、在最该安静时触发。
- **Daydream 原子性**：不可变快照 + 内存 staging + generation 取消 + 最终短 SQLite 事务。语义仍是“全 session 无部分可见结果”，但网络等待期间不占写锁。
- **Daydream 输入归属**：只消化用户自述 impression，不持久化 assistant response。显式记忆/遗忘请求不等待 Daydream。

## 十、运行配置（推荐默认值已落地）

配置位于当前 AI profile 的 `daydream` 对象，例如 `aiSettings.profiles.default.daydream`。模型字段留空时复用 profile 的主模型；`enabled=false` 时不启动空闲监测，也不收集新的 Daydream inbox 印象。所有数值在读取时都会限制到安全范围。

| 配置项 | 默认值 | 语义 |
|---|---|---|
| `enabled` | `true` | 总开关 |
| `idleThresholdSec` / `dueSoonThresholdMs` | `300` / `600000` | 全局空闲阈值 / 待办保护窗口 |
| `minIntervalMs` / `interruptionBackoffMs` | `900000` / `600000` | 最小间隔 / 打断后的额外退避 |
| `hourlyLimit` / `tickIntervalMs` | `3` / `30000` | 每小时 session 上限 / 空闲复查周期 |
| `sessionLimit` / `batchLimit` / `inboxLimit` | `32` / `8` / `200` | 单次、单批和收件箱容量 |
| `relatedMemoryLimit` | `8` | 每批提供给模型的历史候选上限 |
| `model` / `maxTokens` / `temperature` | 空 / `1200` / `0.2` | 独立模型及推理参数 |

平台不支持全局空闲检测时仍默认不触发；原硬编码规则继续作为 LLM 请求失败时的有界降级。被用户交互打断时 generation 立即失效，新对话不等待 Daydream。

## 十一、路线图（Daydream 部分，属主文档 Phase 2）

基本框架（主文档 Phase 1：Embedding/RRF/遗忘/分区迁移）先行，Daydream 在 Phase 2：

| 任务 | 交付物 | 验收标准 |
|------|--------|---------|
| Hippocampus 改造 | `HippocampusCache` 替换 `WorkingMemoryCache` | inbox 语义，待巩固暂存 |
| 检索路径解耦 | 删除 retrieveMemoryHints 同步 cleanup | 检索只读不整理，无 LLM 延迟压入 |
| macOS 空闲补齐 | `get_user_idle_state` macOS 分支（`CGEventSource`） | macOS 可取全局空闲时长 |
| Daydream 新建 | 空闲复合判定 + 快照/staging + generation 取消 + 最终短事务 + LLM Prompt | 空闲异步分类巩固；中断无写入、迟到回调无效、stale snapshot 不提交、正常完成原子清理当前快照 |

---

*本文从 `memory_improvement_plan.md` 拆出，涵盖 Daydream 的定位、触发、中断回滚、巩固流程、改造与决策。基本记忆框架先行落地后再进入 Daydream 实现。*
