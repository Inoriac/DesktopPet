# Task 1 验证记录

日期：2026-10-19。关联系分 §3.2。实现、测试完成；Task 状态和提交由主 agent 审查后更新。

## Sanity check

- `LoadedCapability::toPublicJson()` 输出 ref/kind/instructions/catalogRevision/definitionDigest，Tool 操作带 parameters/result/effects，Skill 带 dependencies，已核实。
- `PluginRuntime::executeGateway(const ToolExecutionRequest&, ToolRuntime::Completion)` 同时供普通工具运行时和内部 reply dispatcher 使用；元数据标识及完整定义上限放在 finish 分支，两条路径均受相同约束。
- `ToolResultSanitizer::sanitize/toPayload` 是原异步结算和消息序列化使用的公共路径；host-only `ToolResult::capabilityMetadata` 默认 false、不序列化，普通结果的原处理不变。
- `AIBrain::thinkInternal(...workingMessages)` 保存原始工作消息，仅 modelRequest 使用投影；`rememberToolOutcome` 的元数据分支保留诊断事件和原始脱敏日志，跳过个人及工作记忆。
- `TaskRecord.steps` 的实际字段是 name/output，output 直接为 ToolResult.data；`TaskOrchestrator::callModel` 统一投影发出副本，原 steps 和 ActionObservation 保持完整。
- 方法级补充已由主 agent 回填系分：Task 门禁在 prepareTool 前，使用原 replan 预算；普通聊天原 3 轮上限调整为 8；普通对话补独立 capability_metadata 日志。
- tests/test_*.cpp + QtTest + CMake 注册符合原目录布局。无新增依赖、用户配置修改或真实模型请求。

## RED → GREEN

| 行为 | RED 证据 | GREEN 证据 |
|---|---|---|
| 9000 字完整说明/Schema 保留；超大定义拒绝 | `/tmp/plugin-context-task1-red-test.log`：原 sanitizer 截断、34000 字仍返回成功 | gateway 三组数据通过，包括 12000 个中文字符的 UTF-8 超限 |
| 公共说明去重、切换回收、原记录/结果不变 | `/tmp/plugin-context-task1-projector-red-test.log`：四例失败，12 份正文未减少 | `/tmp/plugin-context-task1-projector-green-test.log`：6 passed |
| Task 实际请求/恢复投影；缺上下文不派发 | `/tmp/plugin-context-task1-task-red-test.log`：重复正文2份、未见定义仍进入 prepareTool | TaskOrchestrator 全套38 passed |
| AIBrain 实际请求、同响应 load+invoke 必须等待下一请求看见定义 | `/tmp/plugin-context-task1-dialogue-red-test.log`：正文2份、提前 invoke 实际执行 | 新增场景及聊天全套88 passed |
| 顺序 discover/load总览/load操作/invoke；最多8轮 | `/tmp/plugin-context-task1-round-budget-red-test.log`：旧3轮在第4请求中断 | 顺序链完成；持续工具请求在8次执行后终止 |
| 元数据完整归档、请求去重、不进入下一轮记忆 | `/tmp/plugin-context-task1-archive-red-test.log`：完整加载日志0份 | bootstrap 场景验证3份完整load日志、模型1份公共正文、5条工具事件、仅业务调用进入个人/工作记忆 |

最终命令：

```sh
/Users/huang/.qt-build-env/bin/cmake --build build --target capability_context_tests plugin_runtime_tests task_orchestrator_tests streaming_dialogue_tests autonomous_chat_tests module_connectivity_tests -j 4
QT_QPA_PLATFORM=offscreen /Users/huang/.qt-build-env/bin/ctest --test-dir build --output-on-failure -R '^(CapabilityContextTests|PluginRuntimeTests|TaskOrchestratorTests|StreamingDialogueTests|AutonomousChatTests|ModuleConnectivityTests)$'
```

最终结果：6/6 CTest 组通过，32.07 秒。日志 `/tmp/plugin-context-task1-final-ctest.log`。QtTest 计数：CapabilityContext 6、PluginRuntime 24、TaskOrchestrator 38、StreamingDialogue 88、AutonomousChat 5、ModuleConnectivity 42 passed。ModuleConnectivity 原有4项付费真实模型验收未配置，按原条件跳过。自主/模块连通 fixture 无须修改。

## 尺寸实测

同一组合成12次load记录，每份共同说明约2500字符，6次使用video、6次使用browser，各含open/read操作。UTF-8序列化字节数如下（不是token数）：

| 请求材料 | 原始 | 投影后 | 降幅 |
|---|---:|---:|---:|
| 对话消息数组 | 36,843 | 8,455 | 77.1% |
| Task steps数组 | 35,235 | 7,114 | 79.8% |

对话公共说明出现次数12→1；保留最近两个操作的完整Schema。原始消息和steps逐值不变，业务failed/accepted/unknown及tracking/receipt保持原值，assistant tool_calls及transportBlocks保持原值。实际收益依插件正文与任务长度变化，本次未测付费模型token。
