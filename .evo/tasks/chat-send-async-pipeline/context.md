# 上下文

## 需求来源

- `design.md`
- `聊天发送异步流水线-桌面端系分.md`
- 用户在 2026-09-02 对 Windows 平台聊天发送卡顿的验证反馈

## 决策记录

- [2026-09-02] 当前仓库采用集中式异步准备与延迟副作用，不直接迁移完整 Agent Actor。
- [2026-09-02 技术设计自审] §3.2 与 §3.3 均具备完整 7 子节；现有聊天协议与 SQLite schema 不变，后台连接在所属线程内独立创建。
- [code-impl Task 1 sanity check] §3.2 实现锚点 `AIBrain::triggerThink(const QString&, const QString&, const QString&)`、`ModelRouter::completeStreamAsync(const ModelRequest&, LlmStreamObserver, ModelCompletionHandler)`、`MemoryStore::databasePath() const`、`WorkingMemoryCache::all() const` 已验证一致；模型路径在提交准备任务前不访问 SQLite。
- [code-impl Task 1 确认] 用户选择进入 Task 2；Task 1 已通过最终独立复审，未发现剩余 P1/P2。
- [code-impl Task 2 sanity check] §3.3 实现锚点 `EventLedger::append(const EventDraft&)`、`MemoryStore::reinforceEntries(const QStringList&)`、`AiCallLogger::logRequest(...)`、`AgentRuntimeServices::reflectOnCompletedSession(const QString&)` 已验证一致；实现分别复用事件追加、记忆事务、兼容日志值对象和 barrier 后反思。
- [code-impl Task 2 review] 持久化副作用改为有界 FIFO worker；用户记忆和工具结果先在 GUI 线程做纯内存 staging，再以不可变 mutation batch 后台提交，入队失败回滚，避免 SQLite/JSON 写入阻塞模型派发并保持 GUI memory cache 一致。
- [code-impl Task 2 review] 修复关键工作入队失败的显式 warning/回滚、barrier 容量预留、`stop -> start` worker handoff、析构有界关闭，以及取消/迟到/重复 completion 的 response log 去重；README 明确同步工具执行仍是当前架构边界。
- [code-impl Task 2 验证范围] 用户要求以多数场景稳定为准并避免非必要全量测试；本轮仅运行副作用队列、流式对话、准备执行器完整小套件及记忆 staging/强化定向用例，未运行仓库全量测试。
- [code-impl Task 2 确认] 用户确认保留完整延迟副作用实现，并将在 Windows 环境继续进行真实渲染与交互性能验证。

## 设计偏差

- [code-impl Task 1 review repair] 为避免从 legacy memory 路径错误推导 profile/runtime DB，并保持完整 `RuntimeSnapshot`，在 `AgentRuntimeServices` 增加只读 `chatPreparationRuntimeMetadata()`；worker 查询 identity versions 后随准备结果返回。该调整不改变外部协议或数据库 schema。
- [code-impl Task 1 review repair] 为保证显式“忘记”在本轮 prompt 中立即生效，将 `MemoryPolicy` 的 forget 匹配规则提取为纯函数供 worker 过滤只读候选；持久化仍沿用现有 `processUserMemoryWrite()`，未提前实现 §3.3 副作用队列。
- [code-impl Task 2 design extension] 为把既有用户记忆写和工具结果持久化完整移出派发关键路径，副作用类型增加 `UserMemoryWrite`，并增加 `tryEnqueue*` 返回值、`MemoryMutationBatch` 纯内存 staging/rollback 接口；不改变外部聊天协议或 SQLite schema。

## 待回溯设计的发现
