# 上下文

## 需求来源

- `design.md`
- `桌宠流式聊天与Fluent界面重构-桌面端系分.md`

## 决策记录

- [2026-08-27 技术设计自审] `ChatActivityStage` 与 `ChatMessageStatus` 是独立状态轴；阶段变化不持久化为消息状态。
- [2026-08-27 技术设计自审] assistant entry 使用可选 `replyToId` 关联源 user 消息；主动消息和旧迁移记录允许为空。
- [code-impl Task 1 审查] Anthropic provider-native content blocks 必须通过 `LlmResponse::transportBlocks` → `ChatMessage::transportBlocks` 在同一工具请求链内原样续传；已回补 §1.4、§3.2 与 §3.3 契约。
- [code-impl Task 1 sanity check] §3.2 实现锚点 `OpenAICompatibleClient::sendChatCompletionAsync`、`buildMessagesArray`、`LlmResponse::toolCalls` 与 `LlmUsage` 字段已验证一致。

## 设计偏差

## 待回溯设计的发现
