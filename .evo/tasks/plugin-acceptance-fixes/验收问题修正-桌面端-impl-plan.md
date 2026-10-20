# 验收问题修正实现计划

关联设计：[系分](./验收问题修正-桌面端-系分.md)。进度唯一来源。按用户已授权顺序串行推进，每个 Task 完成回归和审查后提交。

## Task 1 完整记忆更正

- 关联设计：§3.1 完整记忆更正
- 状态：[x]
- Wave：1
- 依赖：无
- 文件：core/ai/memory/memory_statement.h；tests/test_memory_attribution.cpp
- 回归：更改星期省略浇水量不能废弃旧记录；同星期的施肥信息不能被浇水改动废弃；主体首句包含未覆盖细节不能废弃；完整更正仍成功、连续更正/人物隔离原用例通过。

## Task 2 加载后暴露具体参数

- 关联设计：§3.2 加载后暴露具体参数
- 状态：[x]
- Wave：2
- 依赖：Task 1（串行检查点）
- 文件：core/ai/plugin/capability_context.{h,cpp}；core/ai/ai_brain_loop.cpp；core/ai/task/task_orchestrator.{h,cpp}；core/ai/ai_brain_tasks.cpp（若宿主覆盖投影需调整）；tests/test_capability_context.cpp；tests/test_task_orchestrator.cpp；现有聊天/插件集成回归；相关 CMake 源文件组
- 回归：加载前无业务 schema；加载后 url 必填且游标可选；两操作独立参数；释放/旧版本不再可用；模型别名映射到网关并保留调用 ID；聊天/任务均走原权限和参数拒绝；原按需预算通过；同 ref 重载后旧投影绑定仍被拒绝。
- 版本绑定补充范围：ToolExecutionRequest 与 core/ai/task/task_types.h、core/ai/plugin/plugin_gateway.cpp、tests/test_plugin_runtime.cpp（若在原网关测试加入回归）。
- 联动补验范围：tests/test_module_connectivity.cpp 的实际自主 runner 回归；模拟规划器也应读取本轮 available_tools，加载前不声明隐藏的 invoke_capability，加载后使用具体操作投影。
- 完成证据：CapabilityContext 8/8、TaskOrchestrator 38/38、PluginRuntime 30/30；StreamingDialogue/AutonomousChat 全目标通过。WebCommand 编译成功，QProcess 启动 QtGui helper 被 macOS AMFI 拦截，详见审查记录；逻辑边界由实际 Python command 网关补验。

## Task 3 跨轮工具上下文

- 关联设计：§3.3 跨轮工具上下文
- 状态：[x]
- Wave：3
- 依赖：Task 2
- 文件：core/ai/ai_brain.{h,cpp}；core/ai/ai_brain_loop.cpp；core/ai/ai_brain_router.cpp；core/ai/chat 下的小型工具会话记录辅助类（若需要）；tests/test_streaming_dialogue.cpp；相关 CMake 源文件组
- 回归：第二轮包含上一轮 URL/nextCursor 及工具调用配对；标准插件仍按需重新投影/校验；两会话不串；清空/切角色丢弃记录；大小预算只删除完整回合；过期/取消回调不写入新会话。
- 完成证据：StreamingDialogue 95/95；正式 submitUserMessage 两轮续读与插件定义投影 RED→GREEN；外部会话隔离、容量、清理及取消回调均通过。

## Task 4 自主选择输出兼容与诊断

- 关联设计：§3.4 自主选择输出兼容与诊断
- 状态：[x]
- Wave：4
- 依赖：Task 3（串行检查点）
- 文件：core/ai/model/model_router.{h,cpp}；core/ai/plugin/schema_validator.{h,cpp}（安全结构诊断）；core/ai/activity/activity_selector.{h,cpp}；core/ai/ai_brain_autonomy.cpp；core/ai/activity/opportunity_store.cpp（仅必要的现有结果字段）；tests/test_model_router.cpp；tests/test_activity_selector.cpp；tests/test_streaming_dialogue.cpp（宿主诊断）；相关 CMake 源文件组
- 回归：合法 JSON 与常见包裹格式可解析；非法字段/引用仍失败；修复请求指出具体结构错误；失败保留安全字段路径和阶段，不暴露原文；合法 start/wait、预算和全局关闭边界不退化；使用 CapabilityRef::toJson 的条目引用省略 operationId 可正常 start，显式操作引用仍可用。
- 完成证据：7/7 相关目标、257 项通过；包含 ModelRouter 44、StreamingDialogue 99、ActivitySelector 25 及自主/预算/运行时回归。结构化输出、真实宿主失败落库和路由归因均有 RED→GREEN。
