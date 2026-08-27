# 上下文

## 需求来源

- `design.md`
- `桌宠流式聊天与Fluent界面重构-桌面端系分.md`

## 决策记录

- [2026-08-27 技术设计自审] `ChatActivityStage` 与 `ChatMessageStatus` 是独立状态轴；阶段变化不持久化为消息状态。
- [2026-08-27 技术设计自审] assistant entry 使用可选 `replyToId` 关联源 user 消息；主动消息和旧迁移记录允许为空。

## 设计偏差

## 待回溯设计的发现
