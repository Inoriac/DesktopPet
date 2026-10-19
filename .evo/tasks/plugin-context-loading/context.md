# 插件按需上下文上下文

## 需求来源
2026-10-19 用户确认上一轮五点方案，并明确“可以，你来做吧”。方案范围：摘要发现、公共说明与操作分离、每请求按需组装、说明不进入生活记忆、保留完整执行日志。沿用此前逐阶段 commit、自主推进的授权；无需重复阶段确认。单仓库 /Users/huang/Desktop/DesktopPet，master，起点 48c3e80，开始时干净。

## 决策记录
- [code-impl Task 2 sanity check] §3.3 锚点 GatewayTool::parameterSchema、PluginRuntime::discoverCapabilities/queryCapabilities/setLearnedSkills、AIBrain::beginActivitySelection、ActivitySelector 已按实际调用验证；原 findGrant 仍决定授权范围，模型只收到排序后的摘要。父级核实真实 AIBrain 轮转测试及 LX Music 原网关兼容。
- [code-impl Task 1 sanity check] §3.2 锚点 LoadedCapability::toPublicJson、PluginRuntime::executeGateway、ToolResultSanitizer::sanitize/toPayload、AIBrain::thinkInternal/rememberToolOutcome、TaskOrchestrator::callModel/dispatchTool 已按源代码验证；step 使用 name/output。父级 review 已核实无全局缓存门禁、协议消息改写或持久 steps 裁剪。
- [2026-10-19] 使用 evoloop-gen-tech-design / evoloop-code-impl。用户已确认方案、实施顺序及自主推进；顺序实现并 review、TDD、Wave 验证，确认不重复询问。
- [2026-10-19] 只修改宿主上下文与发现机制，Plugin/Skill/Tool 格式 1、command/MCP 执行、权限和回执契约保持兼容。无用户配置修改，无真实模型付费调用。
- [2026-10-19] 环境：cmake=/Users/huang/.qt-build-env/bin/cmake，ctest 同目录；Qt 6.8.3，build/；Python launcher/.venv/bin/python；-j 4；QT_QPA_PLATFORM=offscreen。禁止 agent 间并发构建。
- [2026-10-19 Task 2 预检] 摘要候选实际组装位于 AIBrain::beginActivitySelection；setLearnedSkills 定义位于 plugin_gateway.cpp。公共 README 与开发指导位于 contracts/tools/。这些路径已回填系分；轮转测试需覆盖有相关条目时的其余候选，防止步长大于窗口导致漏项。
- [2026-10-19 Task 2 预检] 目录 summary/name 没有 Schema 长度限制。查询用完整目录文本匹配，输出沿用自主候选的 384/128 字符展示上限并加省略号，避免长摘要占满元数据预算；load 不裁剪。已回填系分 §3.3.2。

## 设计偏差
- [2026-10-19 Task 2 CR] 补足中文自然句片段匹配，并将 cursor 绑定内存中的实例生命周期标识：PluginRuntime 重建时数字 revision 会重置，不能仅靠数字认定旧 cursor 属于当前目录。无需持久化新状态；已回填 §3.3。
- [2026-10-19 Task 1] 细化 §3.2 的 Task 上下文门禁位置：前置到 dispatchTool / prepareTool 前，避免未派发调用被误记为 unknown；callModel 保存该请求投影，缺定义用原 replan 预算恢复。已依用户自主推进授权回填系分；不改变对外契约。
- [2026-10-19 Task 1] 集成测试发现原普通聊天最多 3 轮工具，无法完成正常的 discover/load 总览/load operation/invoke 四轮链路。按已授权的完整按需加载目标，调整原上限为 8，沿用原终止机制并补正常/超限测试；已回填 §3.2.3 与 Task 1 文件清单。
- [2026-10-19 Task 1 CR] 普通对话没有持久化完整 workingMessages，原请求日志改为投影后不足以保留每次 load 原文。因此在 rememberToolOutcome 元数据分支用原异步日志队列归档单次已脱敏完整结果，请求日志继续记录实际投影；TaskRecord 不变。已回填 §3.2.3 #8。

## 待回溯设计的发现
无。
