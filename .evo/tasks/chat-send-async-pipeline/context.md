# 上下文

## 需求来源

- `design.md`
- `聊天发送异步流水线-桌面端系分.md`
- 用户在 2026-09-02 对 Windows 平台聊天发送卡顿的验证反馈

## 决策记录

- [2026-09-02] 当前仓库采用集中式异步准备与延迟副作用，不直接迁移完整 Agent Actor。
- [2026-09-02 技术设计自审] §3.2 与 §3.3 均具备完整 7 子节；现有聊天协议与 SQLite schema 不变，后台连接在所属线程内独立创建。
- [code-impl Task 1 sanity check] §3.2 实现锚点 `AIBrain::triggerThink(const QString&, const QString&, const QString&)`、`ModelRouter::completeStreamAsync(const ModelRequest&, LlmStreamObserver, ModelCompletionHandler)`、`MemoryStore::databasePath() const`、`WorkingMemoryCache::all() const` 已验证一致；模型路径在提交准备任务前不访问 SQLite。

## 设计偏差

- [code-impl Task 1 review repair] 为避免从 legacy memory 路径错误推导 profile/runtime DB，并保持完整 `RuntimeSnapshot`，在 `AgentRuntimeServices` 增加只读 `chatPreparationRuntimeMetadata()`；worker 查询 identity versions 后随准备结果返回。该调整不改变外部协议或数据库 schema。
- [code-impl Task 1 review repair] 为保证显式“忘记”在本轮 prompt 中立即生效，将 `MemoryPolicy` 的 forget 匹配规则提取为纯函数供 worker 过滤只读候选；持久化仍沿用现有 `processUserMemoryWrite()`，未提前实现 §3.3 副作用队列。

## 待回溯设计的发现
