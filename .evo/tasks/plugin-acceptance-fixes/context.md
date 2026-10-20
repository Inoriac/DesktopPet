# 需求来源

- 当前用户：“修正一下吧，在这里应该就可以做”。
- `WINDOWS_PLUGIN_ACCEPTANCE_2026-10-20.md`、`MEMORY_FIX_VALIDATION_2026-10-18.md`。
- 2026-10-20 审查确认：不完整更正误覆盖、通用空参数 schema、跨轮工具历史缺失、自主选择无效输出且缺少具体诊断。

## 决策记录

- 用户此前已授权连续推进并阶段性 commit；不再逐 Task 请求确认。真实模型测试留给用户，本地针对性回归支持修复。
- 使用 evoloop-gen-tech-design 明确补丁边界，evoloop-code-impl 串行实现/审查。用户授权优先于技能中的逐步确认要求。
- 工作区起点干净，基线 f7b6e54；Qt 6.8.3/macOS，cmake 与 ctest 位于 /Users/huang/.qt-build-env/bin。
- [设计锚点核查] TaskOrchestrator 实际入口为 callModel/dispatchTool；宿主 requestModel 会覆盖非 LocalOwner 的 available_tools，投影需在该边界保持一致。模型路由器确实仅校验顶层 type/required，选择器再用完整 SchemaValidator 校验；诊断目前仅保留 code。

## 设计偏差

## 实现锚点核查

- [code-impl Task 1 sanity check] MemoryPolicy::discoverRelations 的 correction/replacesCompleteFact 调用、人物/受众前置判断与原事务均核查一致；只改变完整性判断。

## 设计偏差

无。

## 待回溯设计的发现

无。
