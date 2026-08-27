# 上下文

## 需求来源

- `design.md`
- `桌宠流式聊天与Fluent界面重构-桌面端系分.md`

## 决策记录

- [2026-08-27 技术设计自审] `ChatActivityStage` 与 `ChatMessageStatus` 是独立状态轴；阶段变化不持久化为消息状态。
- [2026-08-27 技术设计自审] assistant entry 使用可选 `replyToId` 关联源 user 消息；主动消息和旧迁移记录允许为空。
- [code-impl Task 1 审查] Anthropic provider-native content blocks 必须通过 `LlmResponse::transportBlocks` → `ChatMessage::transportBlocks` 在同一工具请求链内原样续传；已回补 §1.4、§3.2 与 §3.3 契约。
- [code-impl Task 1 sanity check] §3.2 实现锚点 `OpenAICompatibleClient::sendChatCompletionAsync`、`buildMessagesArray`、`LlmResponse::toolCalls` 与 `LlmUsage` 字段已验证一致。
- [code-impl Task 2 sanity check] §3.3 实现锚点 `ModelRouter::completeAsync`、`LlmChatModelClient::completeOnce`、`AIBrain::thinkInternal`、`m_requestGeneration` 与 `toolConfirmationRequired` 续写路径已验证一致；流式扩展保留既有非流式入口和 generation 门禁。
- [code-impl Task 2 确认] 用户接受当前流式编排实现；provider attempt 级 `ModelCallCompleted` 遥测延期到上游契约补充后处理，不阻塞后续聊天功能。
- [code-impl Task 3 sanity check] §3.4 实现锚点 `ProfileMigrationRequest` 的 `profileId/registeredProfileIds/appDataRoot`、`ProfileDataMigrator` 的 profile 数据根、旧 `PetWindow` JSONL 字段与 launcher/C++ QSettings 组织名已验证一致。

## 设计偏差

## 待回溯设计的发现

- [resolved: deferred by user] [code-impl Task 2 审查] §3.3.4 要求 fallback 链中每个 provider attempt 各写一条 `ModelCallCompleted`，但 §3.3.2 的 `ModelRouter::completeStreamAsync` 契约只向 `AIBrain` 暴露最终 completion，当前实现沿用每次 router 调用写一条；用户确认延期到上游系分补充 attempt telemetry 契约后处理。
