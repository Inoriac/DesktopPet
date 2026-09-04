# Phase 4 前置工作总览与指导

> 本文档汇总 Phase 1-3 完成后、进入 Phase 4（Daydream 集成）之前的系列讨论结论，
> 作为后续实施的总览与指导。讨论日期：2026-09-03 / 2026-09-04。

## 当前状态

- ✅ Phase 1（40db6ee）：ActiveMemoryPool、HippocampusWorkingSet、SQLite schema 扩展
- ✅ Phase 2（bc4fe3b）：MemoryCueExtractor、MemoryKeywordIndex、ACTRRanker、retrieveActivated()
- ✅ Phase 3（5c81f91 + eca5f46）：AssociativeActivationEngine（两跳传播 + 人格化探索）、retrieveWithGraphPropagation()
- 测试：27 个召回相关单元测试全部通过（Phase1 7 + Phase2 9 + Phase3 11，此外 memory_strategy_tests 有 7 个与 Daydream commit 相关的预存失败，与召回改造无关）
- ⏳ 未接入生产：新召回入口尚未被 AIBrain / ChatPreparationExecutor 调用
- ⏳ macOS 无 ONNX：embedding 通道以 Noop 优雅退化，词法/激活/图谱三路可用

---

## 一、Daydream 巩固策略：混合模式（规则 + LLM）

**结论**：LLM 不可替代的环节只有**内容合并改写**；但既然必须调用 LLM，
让它同时处理分类/冲突/评分/标签的边际成本极低（批处理 1 条和 8 条的延迟差 ~1 秒），
反而带来准确度提升。因此：

- **正常路径**：LLM 全量决策（现有 Daydream 设计不变，批 8 条、上限 32 条/session）
- **兜底路径**（新增）：LLM 失败/超时/长期未触发时，用确定性规则快速巩固：
  - 分区分类：关键词启发式（喜欢/讨厌→Preference，工具/步骤→Procedural，事实陈述→Semantic，默认 Episodic）
  - 去重：文本相似度 >0.9 → Update（叠 mentionCount，不改写正文）
  - 评分：importance + mentionCount*0.5 + emotionIntensity*3，clamp 0-10
  - 标签：保留原标签 + 主题词典匹配
- **分级触发**（容量驱动）：
  - Hippocampus < 100 条：正常空闲 Daydream（LLM 全量）
  - 100-200 条：放宽空闲判定（如仅需 30s 无对话）+ 混合模式
  - > 200 条：规则快速清理保底，禁止无限积压
- 现有 `DaydreamConsolidator::hardcodedDecisions()` 是兜底雏形，需按上述规则增强

**时间预估结论**（可接受，无需为速度牺牲质量）：
| Hippocampus | 批次 | 总耗时 |
|---|---|---|
| 1-10 条 | 1-2 | 2-9s |
| 10-32 条 | 2-4 | 4-17s |
| 32+ 条 | 多轮 | 每轮 8-17s，轮间隔受触发节流控制 |

依据：桌宠运行时间 >> 交互时间，空闲窗口充足；中度用户日增 ~35 条，
每天 1-2 次 Daydream 即可消化，正常运行不会积压。

## 二、Daydream 手动触发入口

**需求**：除自动触发外，提供用户主动触发（"让桌宠打个盹"）。

- UI 位置：托盘右键菜单 / 设置界面记忆管理页
- 分级反馈：
  - ≤ 32 条：单轮，toast 提示（"开始整理 N 条记忆..."），8-17s
  - 33-100 条：多轮，进度对话框（已整理 x/y 轮）
  - > 100 条：后台分批 + 托盘通知，可继续使用桌宠
- 手动触发跳过空闲判定，但仍走统一 Sleep Cycle 协调（取消/暂存/提交契约不变；
  现有 `SleepTriggerType::Manual` 已预留）

## 三、小憩人格化：Daydream 中断的记忆与表现

**设计哲学**：Daydream 是桌宠的"小憩"，不是后台 GC。被打断是不愉悦的体验，
应转化为人格叙事。

- 技术层（已有）：generation 取消 + staging 丢弃 + ≥10min 退避
- 人格层（新增）：中断时写一条 ShortTerm 记忆进 Hippocampus：
  - summary "小憩被打断了"，tags [daydream, interruption]
  - emotion Sadness，intensity ~0.25（轻微，不夸张），importance ~0.3
- 情绪层（新增）：通过 EmotionEngine 施加轻微负向情绪偏移
- 表现层：无需特殊管道——中断记录自然进入记忆生态，后续 Daydream 由 LLM
  自主决定是否巩固为 Preference（如"用户白天很忙，喜欢即时响应"），
  对话中通过正常记忆召回自然流露（轻度→偶尔提及；累积→委婉表达；
  频繁→可请求"让我先休息完这 10 秒"）

## 四、SleepCycle 自适应触发（不假定用户作息）

**问题**：当前 `SleepPolicy.bedtime = QTime(23,30)` 硬编码，夜猫子/早睡/轮班用户
可能永远无法触发夜间整理。

**方案（分三步实施）**：
1. **立即**：`minimumIdleSeconds` 600 → 1800（30 分钟）；bedtime 限制可配置关闭
2. **短期**：行为驱动触发——连续空闲 ≥30min，或 1 小时窗口内活跃 <5min 且
   Hippocampus 有待巩固项；完全不依赖时钟
3. **长期**：`SleepTimePredictor` 学习用户作息——记录关机时间 + 长空闲起点，
   7 天数据取中位数作为预测就寝时间；积压超阈值时自动降级到行为策略
- 附带风险项：macOS `get_user_idle_state` 未实现（当前仅 Windows），
  这是"Daydream 永不触发"的最大风险源，需按 daydream.md §3.2 用
  `CGEventSourceSecondsSinceLastEventType` 补齐 ⚠️ 高优先级

## 五、渐进式日记：便签（Fragments）+ 夜间缝合 + 崩溃恢复

**核心思想**：白天长空闲时写"便签"（2-3 句片段 + 当时的情绪快照），
夜间/恢复时把便签缝合成正式日记。便签冻结"当时的感受"，
即使深夜才写日记、甚至次日补写，情绪颗粒度不失真。

### 数据模型

```sql
CREATE TABLE diary_fragments (
    fragment_id TEXT PRIMARY KEY,
    profile_id TEXT NOT NULL,
    local_date TEXT NOT NULL,
    segment_index INTEGER NOT NULL,
    body_encrypted BLOB NOT NULL,            -- 复用 PrivatePsycheCrypto
    emotion_snapshot_json TEXT,              -- 当时的情绪快照（保真关键）
    source_from_sequence INTEGER,            -- 事件账本增量游标
    source_to_sequence INTEGER,
    status TEXT NOT NULL DEFAULT 'Draft',    -- Draft | Consumed | Abandoned
    created_at TEXT NOT NULL
);
```

### 流程

- **白天片段**：长空闲触发（可与 Daydream 共享空闲窗口，Daydream 完成后顺手写一段）
  → 收集 [上个片段 to_sequence, 当前) 的事件 + 当前情绪快照 → 素材为空则跳过
  → 小 LLM 调用（~500 token）生成 2-3 句片段 → 短事务原子落库
- **夜间缝合**：读取当天 Draft 片段 + 尾部事件 → LLM 缝合成连贯日记（保留心情变化）
  → 单事务内写 DiaryEntry + 片段标记 Consumed + finalizeSession
- **崩溃恢复**（启动时，复用 SleepTriggerType::Recovery + sessions->incomplete()）：
  - 孤儿 Draft（有片段无对应日记）→ 后台补写缝合，零内容损失
  - incomplete StagingSession → abortSession 丢弃半成品，片段仍是 Draft → 重新缝合（幂等）
  - Consumed 与 DiaryEntry 同事务提交，不存在半状态
- **补写人格化**：补写昨天日记时 prompt 告知"昨晚没来得及写就睡着了"，
  日记开头自然提一句——与小憩中断同一设计哲学：技术状态转化为人格叙事

### 成本

白天 3-5 次小调用（~2000 token/天增量）+ 夜间缝合 1 次（与原一次性方案相当）。

---

## 实施优先级建议

| # | 事项 | 规模 | 依赖 | 优先级 |
|---|------|------|------|--------|
| 1 | ~~macOS 空闲检测（CGEventSource）~~ | ✅ | ✅ | **已完成**（environment_tools.cpp 已实现并链接 CoreGraphics）|
| 2 | SleepPolicy 行为驱动触发（去 bedtime 硬编码） | 小 | 无 | ⭐⭐⭐⭐⭐ |
| 3 | Daydream 中断人格化记录 | 小 | 无 | ⭐⭐⭐⭐（收益/成本比最高）|
| 4 | 巩固兜底：规则增强 + 容量分级触发 | 中 | 无 | ⭐⭐⭐⭐ |
| 5 | 手动触发入口（托盘 + toast/进度） | 中 | #4 | ⭐⭐⭐ |
| 6 | diary_fragments 表 + 片段落库 + 启动恢复检测 | 中 | 无 | ⭐⭐⭐ |
| 7 | 片段收集器（空闲窗口写便签） | 中 | #6 | ⭐⭐⭐ |
| 8 | 夜间缝合改造 + 补写流程 | 中 | #6 #7 | ⭐⭐⭐ |
| 9 | SleepTimePredictor 作息学习 | 中 | #2 | ⭐⭐（观察反馈后再做）|
| 10 | 新召回入口接入生产（AIBrain/ChatPreparation） | 中 | 无 | ⭐⭐⭐⭐（Phase 1-3 的价值兑现）|

完成 #2-#8 与 #10 后进入 Phase 4（Daydream 批次选择优化、混合建图、HNSW 自动更新）。
