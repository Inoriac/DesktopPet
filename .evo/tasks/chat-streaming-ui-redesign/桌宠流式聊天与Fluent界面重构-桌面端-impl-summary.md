# 桌宠流式聊天与 Fluent 界面重构 - 实现总结

## 实现概述

- §3.2：新增 Anthropic Messages 客户端、SSE 增量解析、工具块组装与取消契约，保留 OpenAI-compatible 适配。
- §3.3：ModelRouter/AIBrain 接入流式对话、工具续写、fallback 与 generation 门禁。
- §3.4：建立 profile 级单条连续对话存储、旧历史迁移和阅读位置。
- §3.5：重构可移动 Fluent 聊天窗口，提供连续时间流、上次阅读分割线和自适应输入。
- §3.6：重构桌面快捷输入与流式分段气泡，支持暂停、翻页、长回复分段和多种思考状态文案。
- §3.7：建立 `DEFAULT` 连接档案、角色 `endpointRef` 和独立模型名配置，视觉请求统一经 `ModelRole::Vision` 路由。
- §3.8：建立 `ModelRole::Daydream`，旧 AIBrain 与 sleep adapter 统一使用可独立供应商的轻量模型路由。

## 主要变更

- LLM 协议与编排：`core/ai/llm/`、`core/ai/model/`、`core/ai/ai_brain*`
- 对话存储与 UI：`core/ai/chat/`、`ui/chat_*`、`ui/liquidglasschatbubble*`、`ui/petwindow_*`
- 配置：`launcher/`、`core/configLoader/config_manager.cpp`、`include/ai_types.h`
- Daydream：`core/ai/ai_brain_loop.cpp`、`core/ai/reflection/daydream_sleep_adapter.cpp`
- 验证：`tests/` 下的协议、路由、存储、UI model、分页和 sleep cycle 用例

## 设计偏差

- §3.7 原设计记录了不存在的 `ConfigManager::routeFromJson`；实现按实际文件级 `parseModelRouteConfig` 锚点完成，已经用户确认。
- provider attempt 级遥测需要上游增补契约，已经用户确认延后，不影响当前聊天与 fallback 行为。
- 旧 AIBrain Daydream 路径不为空 `profileId/sessionId` 额外引入共享状态，继续使用现有 generation 门禁；sleep adapter 保留真实事务 session。

## 验证结果

- Task 1-7 均按计划完成，各 Task 的针对性目标均编译成功。
- Task 7 收尾验证：`ModelRouterTests` 与 `LlmTests` 全量通过，4 个新增 Daydream/SleepCycle 用例全部通过。
- macOS 环境不运行 ONNX 相关用例。`SleepCycleTests` 与 `ProfileDataMigratorTests` 全量通过；MemoryEntry 毫秒精度、旧秒级 Daydream change set 恢复与日记密文测试均有回归覆盖。

## TDD 与留痕

- 各 Task 的关键行为均先有具体用例，再完成实现与编译验证。
- 实现锚点、用户确认和已知偏差统一记录在 `context.md`。
