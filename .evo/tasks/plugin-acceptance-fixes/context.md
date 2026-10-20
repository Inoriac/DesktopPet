# 需求来源

- 当前用户：“修正一下吧，在这里应该就可以做”。
- `WINDOWS_PLUGIN_ACCEPTANCE_2026-10-20.md`、`MEMORY_FIX_VALIDATION_2026-10-18.md`。
- 2026-10-20 审查确认：不完整更正误覆盖、通用空参数 schema、跨轮工具历史缺失、自主选择无效输出且缺少具体诊断。

## 决策记录

- 用户此前已授权连续推进并阶段性 commit；不再逐 Task 请求确认。真实模型测试留给用户，本地针对性回归支持修复。
- 使用 evoloop-gen-tech-design 明确补丁边界，evoloop-code-impl 串行实现/审查。用户授权优先于技能中的逐步确认要求。
- 工作区起点干净，基线 f7b6e54；Qt 6.8.3/macOS，cmake 与 ctest 位于 /Users/huang/.qt-build-env/bin。
- [设计锚点核查] TaskOrchestrator 实际入口为 callModel/dispatchTool；宿主 requestModel 会覆盖非 LocalOwner 的 available_tools，投影需在该边界保持一致。模型路由器确实仅校验顶层 type/required，选择器再用完整 SchemaValidator 校验；诊断目前仅保留 code。
- [聊天锚点核查] continuePreparedThink 在主线程接收 worker 已准备好的历史/当前输入；当前输入为 result.messages 最末项。appendToMemory 明确排除协议消息是为防止孤立结果；新增记录应保存完整配对，不改变自然对话窗口规则。既有 triggerThink_afterToolRound_shouldNotLeakToolProtocolIntoNextRequest 回归需改为验证完整配对和游标留存。restoreConversationHistory、clearMemory、setRuntimeServices 均为清理入口。

## 实现锚点核查

- [code-impl Task 1 sanity check] MemoryPolicy::discoverRelations 的 correction/replacesCompleteFact 调用、人物/受众前置判断与原事务均核查一致；只改变完整性判断。
- [code-impl Task 2 sanity check] CapabilityContext::projectMessages/projectSteps/hasOperation、TaskOrchestrator::callModel/dispatchTool、AIBrain 的非 LocalOwner requestModel 覆盖、PluginRuntime::executeGateway 均核查，规范名通过原 ToolRuntime 校验，版本从宿主绑定传递。
- [code-impl Task 3 sanity check] continuePreparedThink 最末消息为当前输入；thinkInternal 的工具回合保存只在 isCurrent 且完整结果返回后执行。clearMemory/restoreConversationHistory/setRuntimeServices 清理，epoch 阻止清理前的回调重新写回。正式 submitUserMessage 路径验证 conversationRevision 水位变化不清历史，身份修订与参与者仍隔离。
- 本机 WebCommand 的 QProcess 启动 QtGui helper 被 AMFI/XProtect 以 exit 9 拦截；直接终端执行正常，重签/重链接未解除。不修改系统安全设置。日志位于 /tmp/desktop-pet-task2-amfi.log；最终复验备注保留部署限制。
- 主 agent 另用 Qt 6.8.3 最小 QProcess 驱动复现 exit 9；把 helper 复制到临时 .app 并标准 macdeployqt 部署仍被拦截。launcher 自带 PySide6/Qt 6.7.3 的 QProcess 可正常执行同一 helper，说明问题与本机启动环境有关。产物位于 build/acceptance-web-macos，仅作诊断；不更改系统设置或替换生产进程调用方式。
- 契约脚本使用已有 /private/tmp/desktop-pet-contract.1v76Kr 依赖执行 96/96；系统 Python 与 launcher venv 未装 jsonschema，不为此次验证新增项目依赖。

## 设计偏差

无。

## 待回溯设计的发现

- [resolved] Task 2 审查发现 loaded 缓存更新可能使旧投影调用采用新版定义，§3.2/§4 已补充宿主内部版本绑定；插件包和业务参数契约不变。
- [resolved] Task 4 预审确认 ActivitySelector::schema 要求 operationId，提示却允许省略；CapabilityRef::toJson 本身也会省略空值。§3.4 改为可选，与条目选择的原业务约定一致。Windows 四次失败是否由此触发仍需真实服务复验。
- [resolved] Task 3 主审发现 conversationRevision 在 acceptMessage 和回复入库时均递增，是正常消息水位；§3.3 修订为不要求该字段相等，仍比较人物修订、参与者及授权。补 submitUserMessage 连续真实宿主轮次回归，避免只有 triggerThink 测试漏掉生产路径。
