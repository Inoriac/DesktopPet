# 自主 Agent 阶段实现总结：§3.2

设计基线提交：`9694046 docs: 记录自主 Agent 设计与实施顺序`。按用户要求每阶段验证后本地提交，不推送。任务进度以同目录 impl-plan 为准；本总结仅覆盖人物识别阶段。

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

§3.3—§3.9 尚未实现。下一项是多人记忆归属、范围召回和存量迁移；QQ、通用自主活动、人物合并/拆分没有通过本轮获得可用能力。本机验证为 macOS Qt 构建/测试，未做 Windows GUI 或真实 QQ 验收。
