# 自主 Agent 阶段实现总结：§3.2—§3.3

设计基线提交：`9694046 docs: 记录自主 Agent 设计与实施顺序`。按用户要求每阶段验证后本地提交，不推送。任务进度以同目录 impl-plan 为准；本总结覆盖已验收的人物识别和记忆归属阶段。

## 已落地行为

PersonDirectory 提供 initialize、resolvePerson、bindIdentity。新增 agent_persons / agent_external_identities，以稳定人物 ID 对应精确外部账号；同名隔离，昵称变化不变更身份，多账号通过宿主已接受事件及人物版本校验绑定。账号冲突不合并历史；创建和绑定失败按事务回滚，绑定事件通过既有 outbox 提交。

旧 owner 和其他 relationship_state subject 保留原 ID/版本。输入显式区分 local_owner、external、internal，外部空对象不会投影到主人。关系效果只写已存在的人物，外部账号即使绑定 owner 也不获得本地主人的人格指导权限。origin_kind 随任务 JSON、经历 outbox 和事件保存，重启后仍保持。

关键入口：
- `core/ai/identity/person_directory.h/.cpp`：目录及事务行为。
- `core/ai/event/sqlite_event_repository.cpp`、`event_schema_registry.cpp`：新增表和绑定事件。
- `core/ai/runtime/agent_runtime_services.*`、`agent_runtime_growth.cpp`：初始化与来源/人物校验。
- `core/ai/chat/chat_preparation_executor.cpp`、`core/ai/ai_brain.cpp`：聊天投影和来源标记。
- `core/ai/task/task_types.*`、`task_store.cpp`、`task_experience_adapter.cpp`：重启与经历来源传播。

## 验证结果

- TDD：目录缺接口的 RED、runtime 10 项错误归属的实际 RED、聊天 external/internal 空对象的 RED、任务重启丢来源的 RED，均转为 GREEN。
- PersonDirectoryTests：22 passed（含数据驱动用例及 init/cleanup）。
- 8 组直接相关测试全部通过：PersonDirectory、AgentRuntimeServices、IdentityState、ChatPreparationExecutor、EventLedger、TaskOrchestrator、TaskExperience、ModuleConnectivity。
- 完整构建成功，包括 Desktop_Pet。
- 最终全量 CTest：**48/48 组通过**。FileWebToolTests 的原有失败已定位为本机 AMFI 子程序签名拦截；macOS 使用系统签名程序验证调用、参数和工作目录，其他平台使用独立 fixture。新增正常非零退出及异常终止验证，工具准确区分这两种结果。
- PromptRendererTests 的旧三项人格精确字符串断言已按现有七项默认值修正并通过。
- git diff --check、八切片/八任务对应关系、测试布局及设计链接检查通过。

验证命令：

```sh
/Users/huang/.qt-build-env/bin/cmake --build build -j8
QT_QPA_PLATFORM=offscreen /Users/huang/.qt-build-env/bin/ctest --test-dir build --output-on-failure -j4
git diff --check
```

本地日志：`build/autonomous-agent-full-build.log`、`build/autonomous-agent-ctest.log`。这些是忽略的构建产物，结论与命令已在本文件留存。

## 设计补全与后续边界

实现前核对的工程细节已同步主设计 §3.2、§4.1、§5.3，详见 context.md；未用实施计划隐式替换契约。

§3.2 的验证为 macOS Qt 构建/测试，未做 Windows GUI 或真实 QQ 验收。


## §3.3 人物记忆归属、召回与迁移

新增类型化归属服务、ContextSpec 及四张记忆关联/迁移表。人物对象与来源发言人分别记录，只有已提交原始事件与宿主解析引用能够确认归属；pending 来源可幂等补全。同人同事实可累积独立来源，不跨人物去重或建立偏好冲突。

写入、召回、图传播、工作记忆、话题缓存、任务摘要、强化和后台巩固均应用人物及会话范围。旧任务与本地主人入口保留受限兼容；写入继续遵守已有敏感信息与置信度策略，损坏归属读取明确失败。MemoryOrganizeTool 与 Daydream 均保留人物、范围和原始证据。

memory schema 升为 v2；旧数据每批 100 条仅补关联，保留正文、ID、分区、强度和时钟，cursor 同事务提交。迁移前恢复原维护批次；正式睡眠 Prepared staging 清理前延后，不放松原版本或 payload hash 校验，不重新调用模型。

主要变更：`core/ai/memory/`、`core/ai/context/context_spec.h`、聊天准备/写队列、AIBrain 聊天与任务材料入口、维护工具、反思巩固服务和相应 tests。

验证：
- 新归属测试 26/26 用例通过（含 init/cleanup 和数据场景），涵盖真实来源、两人隔离、图/临时记忆范围、转述引用、关联故障和 schema/迁移。
- 聊天、写队列、睡眠巩固及归属四个定向套件全部通过；StreamingDialogue 77/77 用例通过。
- 完整应用与全部测试目标重新构建成功，最终 CTest **49/49 组通过**，13.50 秒；无跳过。
- `git diff --check`、八切片与八任务对应、测试布局、行为覆盖映射及实现锚点核对通过。
- TDD 的实际失败与修复记录见审查记录和 context.md。未通过放宽数据归属或删减回归断言取得通过。

最终日志：`/tmp/memory-attribution-final-clean-build.log`、`/tmp/memory-attribution-final-clean-ctest.log`。构建与验证命令沿用上文，最终构建使用 `-j4`。

§3.4—§3.9 尚未完成；下一项是会话输入、上下文准备与回复定位。真实 QQ 和通用自主活动不属于前两阶段的已验收能力。
