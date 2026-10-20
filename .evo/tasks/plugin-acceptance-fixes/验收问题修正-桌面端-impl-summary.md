# 验收问题修正实现总结

关联：[系分](./验收问题修正-桌面端-系分.md)、[实现计划](./验收问题修正-桌面端-impl-plan.md)、[审查](./验收问题修正-桌面端-审查.md)。用户复验入口：[修正与复验](../../../PLUGIN_FIX_REACCEPTANCE_2026-10-20.md)。

## 实现范围

| 设计 | 实现 | 主要文件 |
| --- | --- | --- |
| §3.1 完整记忆更正 | 核对被替换值以外的信息，首句和后续子句都不能丢未覆盖细节；原人物/受众判断与事务保留。 | `memory_statement.h`、`test_memory_attribution.cpp` |
| §3.2 加载后具体参数 | 请求内生成操作工具及业务参数 Schema，执行前归一到原网关；宿主携带定义版本，拒绝旧投影借新缓存执行。聊天与任务两条路径均覆盖。 | `capability_context.*`、`ai_brain_loop.cpp`、`ai_brain_tasks.cpp`、`task_orchestrator.*`、`plugin_gateway.cpp`、工具/任务请求类型及回归 |
| §3.3 跨轮工具上下文 | 仅保存完整调用与结果，每会话两轮、全局 32 KiB；按人物、会话和授权隔离，清理及取消回调有生命周期约束。 | `tool_conversation_history.h`、`ai_brain.*`、`ai_brain_loop.cpp`、`ai_brain_router.cpp`、`test_streaming_dialogue.cpp` |
| §3.4 自主选择兼容与诊断 | 统一结构解析和完整 Schema 校验，接受整体 JSON 代码块；条目引用允许省略操作 ID；修复提示及活动结果包含有界结构诊断。 | `model_router.cpp`、`schema_validator.cpp`、`activity_selector.cpp`、`ai_brain_autonomy.cpp`、`CMakeLists.txt` 和相关回归 |

## 验证方式

四个切片均先用行为回归复现，再修正实现。§3.3 在主审中进一步改用 `AgentBootstrap + submitUserMessage` 真实发送入口，复现了消息水位递增导致历史失效的问题。§3.4 使用正式自主选择失败信号与持久化机会记录验证诊断，检查模型原文不会进入新诊断。

各阶段数量、RED→GREEN 和最终构建/测试结果记录在[审查](./验收问题修正-桌面端-审查.md)。全项目构建成功，最终 72/72 非阻塞测试目标通过、契约 96/96；ModuleConnectivity 的两条旧模拟用例已按新投影更新，并单独补跑全目标通过。测试沿用仓库现有 `tests/` 组织；实现按切片串行审查和提交。未增加生产第三方依赖、数据库表或插件协议版本。

阶段提交：§3.1 `d524a7a`；§3.2 `61d79a1`；§3.3 `0f30343`；§3.4 `d88ee90`。

## 设计修订与边界

审查时已把三项发现回写系分：投影的宿主定义版本绑定；聊天 `conversationRevision` 是消息水位而非受众修订；自主条目引用的可选 `operationId`。详见 [context](./context.md)，没有保留未解决的跨切片设计项。

工具记录只在同次运行内保留，不承诺重启后恢复游标。真实模型的端到端复验留给用户；macOS Qt 6.8.3 启动 QtGui Web helper 受 AMFI/XProtect 限制，不能记为通过。B 站登录字幕、真实 ASR 和图像/视频分析不在本次范围。
