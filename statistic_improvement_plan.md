# 统计系统改造计划

> 基于 2026-07-30 的 code-review 结论制定，作为日后开发指引，非详细实现规范。
> 原则：指出问题在哪、解决思路是什么，具体编码留到实现阶段。

## 0. 核心结论

`StatisticManager` 名义上"基于事件驱动的解耦统计系统"，实际未兑现：

- LLM 用量是其最高频最重要的负载，却**不走事件**，直接锁+改；
- 4 个事件类型里 2 个无生产者、1 个被注释，2 个信号无人监听——**一半抽象面是死的**；
- `QMutex` 只护住了 `petStatisticsMap` 容器，没护住 `PetStatistics` 字段；getter 放出**裸指针**，锁契约被架空；
- 运行时长统计在生产**实质失效**（无 `recordPetStop`、`runtimeUpdateTimer` 从未 `start()`）；
- 落盘非原子，且每次 LLM 回包都在**主线程同步全量写**文件；
- LLM 用量与 `AiCallLogger` **重复记账**。

与系统里较新的子系统（如 `core/ai/memory/` 的 `MemoryRepository` 抽象接口 + DI + 策略分离）存在"代差"。本次做**中等改造**使其自洽，不做全盘 DI 重构。

## 1. 问题清单（按类归并）

| # | 问题 | 位置 | 现象 |
|---|---|---|---|
| C1 | 运行时长统计失效 | `statistic_manager.cpp:273`(死timer) / `:94`(PET_STOP槽) / `main.cpp`(无aboutToQuit) | `totalRuntimeMs`/`sessionRuntimeMs` 不累加，生产永为初始值 |
| C2 | PET_STOP/TOUCH 槽空指针解引用+竞态 | `statistic_manager.cpp:95` / `:111` | `petStatisticsMap[name]` 锁外、无 `ensure`/判空 → `operator[]` 插 nullptr 后 `stats->` 崩溃 |
| C3 | PET_START 槽锁外写字段 | `statistic_manager.cpp:85-88` | 与 `recordLlmUsage` 写者竞态，锁纪律自相矛盾 |
| C4 | getter 放出裸指针 | `statistic_manager.cpp:206-216` → `context_builder.cpp:55,66` | context 读 LLM 字段时与回调写者撕裂读 |
| C5 | 非原子落盘 + 主线程同步写 | `statistic_manager.cpp:309-319` / `:203`,`:168`(即时save) | 崩溃/并发写损坏文件；每次 LLM 回包阻塞 UI |
| C6 | 事件抽象不覆盖 LLM | `statistic_types.h:14-19` / `statistic_manager.cpp:180-204` | 无 `LLM_USAGE` 类型，`recordLlmUsage` 绕过 `emitStatisticEvent` |
| C7 | 死事件 / 死信号 | `statistic_manager.cpp:125`(注释槽) / `:147`,`:90`等(无connect) | `recordPetStop`/`recordTouchInteraction` 无生产者；两个信号无消费者 |
| C8 | LLM 用量重复记账 | `statistic_manager.cpp:189` vs `ai_call_logger.cpp:57-64` | 两条并行活路径，同一 `usage` 两个 JSON schema |
| C9 | `"AI_GLOBAL"` 魔法串重复 | `statistic_manager.cpp:181` / `context_builder.cpp:54` / `test_llm_chat_service.cpp` | 两端字面量各自硬编码，漂移即静默裂分统计 |
| C10 | 重载泄漏 + 测试缺失 | `statistic_manager.cpp:404` / `test_statistic_manager.cpp:203`(注释),`:273`(假并发) | `loadFromFile` 覆盖指针不 `delete`；落盘 round-trip、并发均无测试 |

## 2. 已定决策（实现时遵循）

**改造范围**：中等改造（修正确性 + 让事件抽象自洽，不引全局事件总线、不强行 DI）。
**运行时长**：补 `aboutToQuit` 调 `recordPetStop` + 真正 `start()` `runtimeUpdateTimer`。
**持久化**：原子写(临时文件+rename) + 去掉每 LLM 调用同步落盘 + 定时(60s) + `aboutToQuit` 最终落盘。
**LLM 去重**：保留两套职责（Statistic=聚合 / AiCallLogger=明细），只抽共享 codec/常量。

补充确认：
- 触摸生产者入口已定（见 §4.1）。
- **有统计展示计划**：在 Python launcher 侧做简单展示 → 保持 `log/statistics.json` schema 稳定/向后兼容，launcher 读该文件（见 §4.2）。
- `aboutToQuit` 用 lambda 捕获 `petName`。
- `LlmUsage` 内嵌进 `StatisticEvent` 可接受。
- `EMOTION_INTERACTION` 枚举保留 + TODO，情绪系统未实现。
- `getInstance()`→`instance()` 命名对齐**单独一步**专门做（影响所有调用点）。

## 3. 改造阶段（每阶段可独立编译/测试/提交）

### Phase 1 — 正确性稳底（不引接口变化）
- **C3 锁纪律**：把 PET_START 槽写操作移进锁内，与 `recordLlmUsage` 一致。
- **C2**：PET_STOP/TOUCH 槽改为锁内 `ensurePetStatistics` + 判空后操作。
- **C4 getter 快照**：`getPetStatistics` 返回 `PetStatistics` 值、`getAllPetStatistics` 返回值容器；同步改 `context_builder.cpp:55-67` 与测试调用点（getter 返回类型变化是本阶段破口，需一并改）。
- **C1 运行时长**：`main.cpp` 注册 `aboutToQuit` → `recordPetStop(petName)`；`initialize()` 内 `runtimeUpdateTimer->start(1000)`；`recordPetStart` 重置 `sessionRuntimeMs=0`。
- **C5 落盘**：`saveToFile()` 改临时文件 + `QFile::rename` 原子替换；删 `recordLlmUsage`/`recordPetStop` 的即时 save；`onAutoSaveTimer` 60s + `aboutToQuit` 最终落盘 + 脏标记避免空写。

### Phase 2 — 事件抽象自洽
- **C6 LLM 走事件**：`StatisticEventType` 增 `LLM_USAGE`；`StatisticEvent` 内嵌 `LlmUsage`；`recordLlmUsage` 改为构造事件 → `emitStatisticEvent`，在 `initialize` 注册 `LLM_USAGE` 槽做原累加。借此整理 `StatisticEvent` 构造（现 `areaName` 复用为 detail 字段语义混乱）。
- **C7 死事件补生产者**：PET_STOP（Phase 1 已补）；BODY_PART_TOUCH 见 §4.1。
- **C7 死信号**：`statisticsUpdated`/`statisticEventOccurred` 不删，保留并加注释"预留 launcher 展示接入"。
- **C9 共享常量**：抽 `kAiGlobalStatsKey`，三处统一引用。

### Phase 3 — 去重与命名
- **C8 共享 codec**：将 `ai_call_logger.cpp:57-64` 的 `LlmUsage → QJsonObject` 提为共享函数（或 `LlmUsage::toJson()`），两路共用，不合并存储。
- **命名对齐**（单独一步）：`getInstance()` → `instance()`，对齐 `ConfigManager`/`Pet`/`ThemeManager`，重命名全部调用点（`main.cpp` / `context_builder.cpp` / `llm_chat_service.cpp` / 测试）。

### Phase 4 — 测试补齐
- **C10**：启用/重写 `testSaveLoad`（round-trip，含原子写）；`testConcurrentAccess` 改真并发（`QThreadPool` 多线程 `recordLlmUsage` + `getPetStatistics`）；新增 `aboutToQuit` → stop 结算测试。

## 4. 实现时需注意/确认

### 4.1 触摸生产者入口（已定位）
`ui/petwindow_interaction.cpp`：
- `mousePressEvent`(:173) 射线命中得 `hitPartTag = engine->checkHit(...)`；
- `mouseReleaseEvent`(:279) 调 `triggerTouchReaction(hitPartTag)`(:129)。
- **位置**：在 `triggerTouchReaction(const std::string& tag)` 内（`canTriggerTouch()` 守卫通过后）调 `StatisticManager::getInstance().recordTouchInteraction(petName, QString::fromStdString(tag))`，`tag` 即部位名（`TouchHead/TouchBody/TouchHandL/TouchHandR/Happy`，源于 `config_manager.cpp:263`），作为 `areaName`。
- 注意 `petName` 在 `PetWindow` 内如何获取，避免空名回退到 `AI_GLOBAL` 与触摸语义不符。

### 4.2 launcher 展示与 schema 兼容
- launcher（`launcher/app_state.py` 等）将读 `log/statistics.json` 做简单展示。
- 改造期间保持该文件 schema **向后兼容**：增字段可，勿删/改既有字段语义；version 号随升级。
- 若 launcher 需要运行时长实时刷新，落盘频率需兼顾（60s 可能不够"实时"），届时再评估是否加更细的导出通道或在 launcher 侧自行估算。

### 4.3 其它
- `aboutToQuit` lambda 捕获 `petName`：注意 `main.cpp` 中 `petName` 为栈变量，lambda 需按值捕获；若 `PET_START` 未调过（`startTime` 无效），PET_STOP 槽判空跳过（Phase 1 已加判空）。
- getter 改值拷贝后 `context_builder` 每回合一次读取，成本可接受；确认无其它高频读取点。
- `LlmUsage` 内嵌后 `StatisticEvent` 体积变大，事件分发走拷贝，低频可接受。

## 5. 不在本轮范围 / 风险
- **不做**：抽象 `IStatisticRepository` 接口、全盘 DI、全局事件总线（属深度重构档）。
- **风险**：
  - Phase 1 getter 返回类型变化破坏 context_builder 与测试调用，需同改。
  - 去掉即时落盘后极端崩溃丢 ≤60s 统计；不可接受则回退为"后台异步原子落盘"。
  - `LlmUsage` 内嵌 `StatisticEvent` 会牵动事件构造与所有 `emitStatisticEvent` 路径，Phase 2 改动面相对集中。
- 回滚粒度 = 阶段；建议按 Phase 1→4 顺序提交。