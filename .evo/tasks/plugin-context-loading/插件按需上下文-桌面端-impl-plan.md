# 插件按需上下文实现计划

设计：[系分](./插件按需上下文-桌面端-系分.md)。本文件为进度唯一来源。用户已确认方案及自主按阶段提交；沿用授权完成两个 Task 后总体验证。两个 Task 同一 Wave 内按依赖顺序串行，结束时做完整回归。

## Task 1：已加载能力的请求投影与记忆边界
- 关联设计：§3.2
- 状态：[x]
- Wave：1
- 依赖：无
- 文件：设计 §2 中 capability_context、ai_types、sanitizer、plugin_gateway（只 metadata 标记）、ai_brain.h/loop/router、task_orchestrator、ai_brain_tasks（必要）、CMake、tests/test_capability_context.cpp、tests/test_streaming_dialogue.cpp、tests/test_task_orchestrator.cpp、tests/test_plugin_runtime.cpp。
- 测试：公共说明去重；切换回收/reload；Schema 长字段/敏感参数名完整；超限整项拒绝；原日志和签名消息不变；task modelContext/恢复投影；failed/accepted/unknown 保留；真实 AIBrain 元数据不入记忆；顺序 discover/load 总览/load operation/invoke 及轮次上限；请求尺寸实测。
- 验证：相关 QtTest，父级 review；提交由主 agent 执行。
- 证据：[Task1-验证记录](./Task1-验证记录.md)；父级 CR 通过，6/6 相关 CTest 组通过。

## Task 2：摘要查询、分页与自主候选
- 关联设计：§3.3
- 状态：[ ]
- Wave：1
- 依赖：Task 1
- 文件：capability_discovery、plugin_runtime、plugin_gateway、ai_brain_autonomy、ai_brain.h、tests/test_plugin_runtime.cpp、tests/test_activity_selector.cpp（必要）、CMake、公共 contracts/tools 文档。
- 测试：30 条多页；中英查询；过期/范围/坏 cursor；learned 快照版本；后排相关条目；自主轮转覆盖；真实旧插件 gateway 回归。
- 验证：相关 QtTest，父级 review；Wave 全项目 build / CTest、契约脚本和适配器校验；提交由主 agent 执行。

## Wave 验证
- [ ] 串行文件冲突核查
- [ ] 完整构建与全量 CTest
- [ ] 设计行为覆盖映射
- [ ] tests/test_*.cpp 位置及 CMake
- [ ] 实现锚点与 context sanity 留痕
