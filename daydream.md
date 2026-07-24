# Daydream — 桌宠空闲记忆整理设计

> 从 `memory_improvement_plan.md` 拆出。基本记忆框架（Embedding 检索 / RRF 融合 / 自适应遗忘 / 分区迁移）先行落地，Daydream 作为第二阶段独立实现。本文为 Daydream 的完整设计。

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

当前**没有** Daydream。巩固是在 `ai_brain_router.cpp:272` 检索路径里**同步** `cleanup()`，会把延迟压到用户每次提问路上；且固定写 `Episodic`、无分区/无冲突判定。本设计是**新建**异步 Daydream，而非"升级已有"。当前 `AgentScheduler` 仅有 `QTimer + checkDueTasks`（只跑 ScheduledTask 定时事项），可作为 Daydream 触发底座之一，但还需引入「用户空闲检测」。

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

单 session 总量上限（如 ≤ 32 条待巩固印象，分批每批 ≤8），保证 session 时长有界——这是"中断时全 session 回滚可接受"的前提（见第五节）。处理期间锁住待巩固区，防止与检索并发写。

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
    // 按 turn/时间排序
    std::sort(memories.begin(), memories.end(), byTurn);

    // 分块：每块适配 LLM 上下文窗口（对齐 hebb: chars = (max_tokens - 2000) × 4）
    constexpr int CHUNK_SIZE = 8;
    for (size_t i = 0; i < memories.size(); i += CHUNK_SIZE) {
        auto chunk = slice(memories, i, CHUNK_SIZE);
        auto decisions = callLLM(chunk);   // 一次 LLM 调用处理一批
        applyDecisions(decisions, db);     // 写入同一事务（见第五节）
    }
}
```

**Token 成本估算：** 每 8 条印象一次调用，单次约 1000-2000 token；空闲触发、低频、可配置开关。

## 五、中断回滚与事务原子性（核心需求）

作者要求：Daydream 运行中一旦滑入非空闲，**打断操作并回滚到触发前的记忆状态**，杜绝"数据整理时的图残留"和"协程中断导致的信息残留"。这映射为**事务原子性 + 协作式取消**。

### 5.1 单事务包裹整个 session（全 session 回滚语义）

整个 Daydream session 的所有写入——删 Hippocampus 源记忆、写目标分区、建共现边、改 `memory_relations`——在同一 SQLite 事务内执行，session 全部完成才 `COMMIT`；被打断则 `ROLLBACK`，主库回到触发前。

```cpp
// QSqlDatabase 单连接（m_connectionName）已支持 transaction/commit/rollback
db.transaction();
try {
    for (auto& chunk : pendingChunks) {
        if (m_cancelToken.isCancelled()) throw DaydreamInterrupted{};
        auto decisions = co_await callLLM(chunk);   // 异步，回调检查取消令牌
        applyDecisions(decisions, db);               // 写入同一事务
    }
    db.commit();
} catch (...) {
    db.rollback();   // 主库回到触发前，源记忆恢复、新记忆消失、无图残留
}
```

- **关系图无残留**：`memory_relations` 已是 SQLite 表，随事务回滚。
- **共现图必须纳入事务**：新建的 `TagCooccurrenceGraph` 落 SQLite 表（`tag_cooccurrences`），而非纯内存对象——否则内存图无法随 `ROLLBACK` 撤销，就是"图残留"的根因。共现图 = SQLite 表 + 同事务写入（见主文档 E 节双图架构）。
- **回滚语义取舍**：用户要的是"回滚到触发前那一刻"=全 session 语义，故不采用批级提交（批级提交被打断会保留已完成批，与需求不符）。代价是 session 很长被打断则前功尽弃——通过节流（session 总量 ≤32、空闲期通常够跑完）把代价控制在可接受范围。

### 5.2 协作式取消令牌（防"协程中断信息残留"）

LLM 调用走 `ChatService::requestAsync` 异步回调，强杀网络线程不现实；"残留"风险是「取消后才到达的回调把部分结果写进库」。三重保障：

- **取消令牌**：session 启动时分配 `m_cancelToken`；空闲监测发现滑入非空闲 → `cancel()`。
- **回调丢弃**：每个 LLM 批次回调到达时先检查令牌；已取消则丢弃整批结果，不调 `applyDecisions`、不落库。
- **事务回滚**：批循环捕获 `DaydreamInterrupted` → `db.rollback()`；即便有遗漏的批已写入事务，`ROLLBACK` 一并撤销。

> 这三道缺一不可：只靠令牌不回滚→中间批可能已写；只靠回滚不检查令牌→回调仍会踩进已回滚的事务后再次写入。令牌挡"新回调落库"，事务挡"已写但未提交"，二者联动才无残留。

### 5.3 并发写保护

session 运行期间，`TagCooccurrenceGraph` / `memory_relations` 的写入由该事务独占；同时日常对话若产生新 Hippocampus 项，写到 inbox 表（不参与当前事务），下次 Daydream 再取，不与在途事务争锁。被打断时有新对话进来 → 新对话优先：Daydream 先 ROLLBACK 让出，新对话不等。

## 六、Working Memory → Hippocampus 改造

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
| **运行中滑入非空闲** | 取消令牌置位 → 丢弃在途 LLM 回调 → `db.rollback()` → 主库回到触发前，无图/协程残留，session 前功尽弃但状态一致 |

原硬编码巩固规则（`mentionCount≥2` / `emotion≥0.7`）并存到 Phase 3，作为 Daydream 失败降级兜底，验证稳定后删。

## 八、风险

| 风险 | 影响 | 应对 |
|------|------|------|
| Daydream LLM 巩固增加 token 消耗 | 成本上升 | 空闲低频触发（每批 ≤8 条），可配置开关 |
| 检索路径解耦后遗漏清理 | inbox 堆积 | Daydream 必须有兜底定时触发，不能只依赖空闲 |
| 空闲检测误判（如用户离开但进程未空闲） | 巩固抢资源/频繁打断 | 复合判定（全局空闲+m_busy+待办）+ 最小空闲时长 + session 总量上限 + 打断后退避 |
| macOS/Linux 无全局空闲实现 | 非平台无法触发 Daydream | macOS 补 `CGEventSource`（Daydream 前）；Linux 后置；不支持时默认不触发，可选降级判定 |
| Daydream 中断回滚不彻底（图/协程残留） | 半修改状态污染图谱、漏写 | 全 session 单事务 ROLLBACK + 取消令牌丢弃在途回调 + 共现图落表纳入事务（见第五节） |
| SQLite 长事务持锁 | 阻塞日常对话写入 | session 总量上限（≤32）控制事务时长；inbox 写入与在途事务不争同一行锁 |

## 九、已定决策

- **Daydream 输入口径**：用**系统级全局空闲**（`get_user_idle_state`，即日常使用输入，非 pet 窗口输入）；macOS 补 `CGEventSource`，Linux 后置，不支持时默认不触发。理由：只看 pet 输入会把"用户忙碌工作"误判为空闲、在最该安静时触发。
- **Daydream 中断回滚**：全 session 单 SQLite 事务 ROLLBACK + 协作式取消令牌；共现图落 `tag_cooccurrences` 表纳入事务。语义=回滚到触发前。

## 十、待确认（已带推荐默认值，可逐条调整）

| # | 项 | 推荐默认 |
|---|---|---|
| 1 | 空闲阈值 N₁ / N₂ / 间隔下限 / session 上限 | 5min / 10min / 15min / 32 条 |
| 2 | 打断后退避（防抖动） | ≥10min 不再判定空闲 |
| 3 | 中断监测 tick 频率 | 每 30s 复查 idle_seconds 跳变 |
| 4 | 平台不支持时降级判定 | 默认关闭；需配置显式开（仅 m_busy+无待办+距上次对话） |
| 5 | Daydream 用哪个 LLM | 复用 ChatService（OpenAI-compatible），单独配轻量模型，可开关默认开 |
| 6 | 被打断时有新对话进来 | 新对话优先：Daydream 先 ROLLBACK 让出，新对话不等 |
| 7 | 每小时 Daydream 上限 | ≤3 次/小时 |
| 8 | 原硬编码巩固规则保留期 | 并存到 Phase 3，作为 Daydream 失败降级兜底，验证稳定后删 |

## 十一、路线图（Daydream 部分，属主文档 Phase 2）

基本框架（主文档 Phase 1：Embedding/RRF/遗忘/分区迁移）先行，Daydream 在 Phase 2：

| 任务 | 交付物 | 验收标准 |
|------|--------|---------|
| Hippocampus 改造 | `HippocampusCache` 替换 `WorkingMemoryCache` | inbox 语义，待巩固暂存 |
| 检索路径解耦 | 删除 retrieveMemoryHints 同步 cleanup | 检索只读不整理，无 LLM 延迟压入 |
| macOS 空闲补齐 | `get_user_idle_state` macOS 分支（`CGEventSource`） | macOS 可取全局空闲时长 |
| Daydream 新建 | 空闲复合判定 + 单事务 ROLLBACK + 取消令牌 + `ConsolidationService` + LLM Prompt | 空闲异步分类巩固，inbox 被清空；**中断回滚测试通过**（打断后图谱/记忆回到触发前，无残留） |

---

*本文从 `memory_improvement_plan.md` 拆出，涵盖 Daydream 的定位、触发、中断回滚、巩固流程、改造与决策。基本记忆框架先行落地后再进入 Daydream 实现。*