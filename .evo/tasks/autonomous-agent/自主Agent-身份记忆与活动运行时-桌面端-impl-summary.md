# 自主 Agent 阶段实现总结：§3.2—§3.9

设计基线提交：`9694046 docs: 记录自主 Agent 设计与实施顺序`。按用户要求每阶段验证后本地提交，不推送。任务进度以同目录 impl-plan 为准；本总结记录人物识别、记忆归属、会话路由、插件与活动运行时，以及空闲机会调度、自主活动选择、外部观察与聊天接入的实现及验证。

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

## §3.4 会话输入、上下文准备与回复定位

新增 ConversationStore 与异步 ConversationService。会话、消息、发送绑定和回执进入 runtime SQLite；桌面生产入口迁移旧 JSONL 后只读写会话库。迁移每批 100 条并保存 cursor，保留旧 ID/回复链/状态；已完成迁移不依赖旧文件可读性，存储失败不回退旧文件。

AIBrain 按会话排队、准备权威历史并固定本轮 ReplyTarget。外部内容不进入桌面聊天记录，群聊逐条保留宿主核验的 speaker/message 标识。没有范围授权的旧工具与学习文本不向外部投影；受控发送使用实际 ToolRuntime 异步执行，重复或未知状态不重发，取消派发前窗口受 token 保护，真实迟到回执仍归档原会话。

MessageObserved 是唯一原始消息事实；会话处理日志保留多来源引用，关系成长与反思不会再把处理日志算成新证据。历史导入与自身回显不重放成长；人格和日记使用统一的本地主人来源门禁。outbox 延后投递可恢复，同一原始来源仍可在 cutoff 内回溯。

主要变更：core/ai/chat/conversation_*、历史存储与准备/写队列、AIBrain 路由、范围投影、事件 schema/原始来源类型、运行时成长/人格、睡眠库版本兼容、生产 UI 的 runtime 路径和对应测试。

验证：
- 20 个路由行为测试覆盖远端/双 worker 去重、群聊身份、原目标、真实回执、取消窗口、存储故障、分批迁移及界面不被写锁阻塞；另有生产聊天、历史、人格/关系、schema 的集成回归。
- 真实 RED→GREEN 包括缺接口、历史水位、取消派发、源 alias 投递顺序、已迁移历史读取等；旧库 fixture 修正明确版本，保留数据不变断言。
- 完整应用和所有测试目标构建成功，最终 CTest **50/50 组通过**（42.64 秒）；未跳过测试。`git diff --check`、覆盖映射、测试布局、8 切片与 8 Task 对应及实现锚点核对通过。
- 最终日志：`/tmp/conversation-routing-full-build.log`、`/tmp/conversation-routing-full-ctest.log`。

## §3.5 插件发现、按需加载与通用执行

新增 Plugin/Skill 清单与标准 JSON Schema 2020-12 校验器，保留 Tool 格式 1。PluginCatalog 支持纯 Skill、纯 Tool、混合包、摘要发现、依赖核对、按需正文/参数加载及版本与内容摘要失效。学习文本通过 SkillStore adapter 接入，限定本地主人范围。

PluginRuntime 使用每个 profile 的 plugins.json 管理安装、启停、配置、精确授权和结果映射。command 通过 argv/JSON stdin 执行，MCP 使用实际 stdio 初始化、枚举与 tools/call；完成、已受理、失败及结果未知分别返回。secret 只用于执行绑定，回执按本次配置快照脱敏。

ToolRegistry、PolicyEngine 与任务执行采用显式宿主元数据、工厂和异步绑定，去掉工具名称构造分支。生产 AIBrain 聊天与任务共用异步 ToolRuntime，三个通用入口负责发现、加载和调用。中途确认后继续本轮剩余工具；取消、禁用和替换阻止未派发调用，在途回执保留原身份。

宿主回复绑定将固定会话目标与远端消息引用装配为操作参数，验证实际远端 ID、后端正文和发送时间。ConversationService 与执行器共用派发 token，加载时取消不会继续发送；回执使用持有原库路径的独立后台队列保存，服务销毁后不丢弃已发生事实。

主要变更：core/ai/plugin、ai_brain_plugins.cpp、工具 registry/runtime/policy、MCP client、TaskOrchestrator/执行适配、ConversationService、UI 注册、contracts/tools、third_party/jsoncons 与相应 tests。接入指南：contracts/tools/插件运行时接入.md。

验证：完整应用及全部测试目标构建成功；最终 CTest **51/51 组通过**（64.02 秒），Python 契约 **70/70 通过**，Wave 4 五项校验与 git diff --check 通过。TDD 记录与 14 项覆盖映射见同目录审查记录；日志 /tmp/task4-final-full-build.log、/tmp/task4-final-full-ctest.log、/tmp/task4-final-contract.log。

## §3.6 活动执行、挂起、恢复与经历

新增 ActivityStore 与 ActivityRuntime。runtime SQLite 升至 schema 3，旧任务及经历按原 ID 幂等接管；旧写入口在打开及写事务内拒绝新库。活动拥有一个前台位置，可保留多个 Ready/等待/暂停实例；状态、动作记录、累计预算及经历 outbox 以 SQLite 为权威。

生产 runner、store、计时器位于专属串行 worker；AIBrain 使用快照与异步命令，模型/工具请求携带原活动和会话范围。开始与恢复使用当前插件版本/启用/授权校验；实际派发再次检查权限。发送后的真实消息锚点或 accepted 跟踪条件允许释放前台；恢复核对已提交事件，原子保存另一活动进度。用户和重启暂停须明确恢复，预算不会重置。

写动作记录在外部派发前提交，未知或成功动作不重发；迟到成功与失败均绑定原实例，执行器销毁后真实回执仍归原库。完成前必须没有未结算动作并具备成功证据；插件真实 data 中的空查询可完成但不写长期经历。类型化活动的经历保留参与人、会话、来源与受众，无明确 subject 时不虚构发言人。

主要文件：core/ai/activity、TaskStore/TaskOrchestrator/TaskExperienceAdapter、ai_brain_tasks.cpp、PluginRuntime 校验器、ConversationStore 当前范围读取、runtime schema 与对应 tests。完整回归还发现并修复 ChatSideEffectQueue 退出时的 worker 销毁竞态；旧主动聊天保留到明确自主开关启用。

验证：最终完整应用及所有测试目标构建成功；CTest **52/52 通过**（69.25 秒），Python 工具契约 **70/70 通过**。Wave 5 五项校验、设计结构/链接/SQL 演练与 git diff --check 通过。TDD 实际失败包括多活动、迟到回执、真实结果形状、恢复人物范围、未结算完成、无 subject 参与人及析构崩溃，详见审查记录。

最终日志：/tmp/task5-handoff-build.log、/tmp/task5-handoff-ctest.log、/tmp/task5-final-contract.log。下一项是 §3.7 空闲判定与自主机会调度；真实 QQ 仍未连接。

## §3.7 空闲判定与自主机会调度

新增 RuntimeReadiness/IdleDecision、OpportunityStore 和 OpportunityCoordinator，复用活动运行时的单 worker。是否空闲与运行、启用、暂停、正式睡眠、模式、冷却及预算分别判断；前台活动和选择预留互斥。消息按真实会话聚合，1 秒静默窗口、3 秒最长等待。没有选择器时只做本地检查，不预留机会或调用模型。

aiSettings.autonomy 默认关闭；Launcher 提供自主启用和暂停。启用后原主动聊天、idle 和屏幕自动触发转入通用机会。机会租约、输入水位、宿主 epoch、退避及预算保存在现有 runtime SQLite，重启和对象重建保持用量并拒绝旧控制命令。

每次自主模型实际发送（包括 fallback 和格式修复）先持久预留，再发送、再按原 profile 结算；未知用量保守计费，迟到有效 usage 可补实，真实超额阻断下一次请求。选择与自主活动共用 token/预估费用上限；活跃执行按单调时钟累计，等待不计，耗尽后暂停。明确用户任务仍沿用原规则。

主要文件：core/ai/scheduler/opportunity_types.*、core/ai/activity/opportunity_* 与 activity_runtime/store.*、ModelRouter、TaskOrchestrator、AIBrain、ConfigManager 与 Launcher。全量回归修复 ModelRouter 重复完成回调崩溃，以及 runtime/身份/事件检查点 deferred CAS 的写竞争；采用一次业务完成门及 BEGIN IMMEDIATE，保留重复回执补实和原版本校验。

验证：完整构建成功，CTest **54/54 组通过**（72.34 秒）；机会套件 26/26、运行时服务 57/57、流式对话 79/79，工具契约 70/70。三个数据库竞争用例分别连续 50/50/100 次通过；TDD、覆盖映射、实现锚点和五项 Wave 校验见计划与审查记录。日志为 /tmp/task6-final-verified-build.log、/tmp/task6-final-verified-ctest.log、/tmp/task6-final-contract.log。

第 7 阶段接入真实自主选择，第 8 阶段验证外部观察及消息流程；本阶段未接通真实 QQ。

## §3.8 自主活动选择与持续意图

AIBrain 安装真实 ActivitySelector consumer，使用原 ModelRouter 和按实际发送结算的预算 hooks。主体从有范围的记忆、真实新消息、未解内心线索、相关人物关系与能力摘要提出 start/resume/continue/wait；原子采用后激活现有 TaskOrchestrator。材料和引用有界，空材料允许休息，模型故障与休息分开。

新增 Internal 显式自主工具 grant 和独立 memoryAccess 配置；默认不允许主人/外部会话私有记忆。真实 scope 与自主属性贯穿发现、加载、校验和调用，外部机会重新准备自己的上下文。恢复保留原预算、人物、检查点及人格版本，撤销授权后禁止旧 Internal 意图恢复；用户/重启暂停仍由用户恢复。

重要回复先保存计划与实际结果再切换；异步工具准入及最终派发前均检查 checkpoint，保存失败不会再派发。真实只读查询才更新 lastCheckedAt，只有空查询的自主活动完成时不生成对话经历。配置失败、暂停、预算与任务结束状态从权威路径返回。

主要文件：core/ai/activity/activity_selector.*、selection_types.h、ai_brain_autonomy.cpp，及原 ActivityStore/Runtime、机会账本、插件授权、任务执行器、私有内心读取/身份投影。新增 tests/test_activity_selector.cpp，回归沿用既有模块、插件、任务与配置套件。

验证：完整构建成功，CTest **55/55 通过**（75.44 秒）；选择器 21/21、执行器 30/30、工具契约 70/70。真实两套包/操作名、权限撤销、同 ID 记忆改义、原子采用 SQL 回滚、fallback/修复计费、空查询归档和检查点故障均覆盖。真实 RED 与 GREEN、覆盖映射和 Wave 7 记录见审查文档；没有未同步设计偏差。

日志：/tmp/task7-final-build.log、/tmp/task7-final-ctest.log、/tmp/task7-final-contract.log；报告 /tmp/task7-implementation-report.md。此阶段使用受控后端，不代表真实 QQ 已接通。


## §3.9 外部观察与 QQ 式自主使用验证

新增宿主 observationBindings/observationAdapters、可选 source schema 和独立进程 worker。实际 completed 查询与 source 消息共用稳定人物、消息和原事件路径；消息/outbox/来源 marker/线索在原 runtime SQLite 同事务归档。每次选择提供至多 8 条实际线索，剩余线索在忙碌、暂停、重启与 Internal tick 后仍保留。

宿主明确区分 Query 与 Source。分群查询不推进整个账号的监听游标；只有有序 source 提交后确认水位，从而避免查到群 A 的高序号后漏掉群 B 的旧消息。64 KiB 单行、256 队列、有界读缓冲与诊断、失败头事件保留、崩溃重连和目录代次均受控；错误/损坏游标不默认为空。unknown 原始时间贯穿消息、事件和记忆，memory_origins.original_at 保持 NULL。

直接自主发送复用 replyBindings 和既有派发记录；已有 ConversationService 回复携带宿主 prepared 引用，兼容没有原生 reply-to 参数的接口。实际回执在公开结果脱敏前提取，归档后才推进 runner；写入失败保留原回执补归档，服务退出后仍归原库，不能重发修复。退役实例不再给新主体投递机会。

真实规划增加互斥 wait 提案，由宿主从实际步骤解析消息或远端任务锚点，进入 WaitingExternal 并释放前台。完整等待匹配后才限量获取候选，普通消息与历史洪流不会挤掉真回复。自主匹配等待经 Ready、选择票据及当前权限恢复，用户意图仍保留 Event 授权路径；恢复保留人物范围、进度、原证据及累计预算。

交付 [受控聊天示例](../../../contracts/tools/examples/controlled-chat/README.md) 和 [观察源接入指南](../../../contracts/tools/观察源与聊天插件接入.md)。示例包含 list/read/send/receipt、可选 JSONL source 与宿主配置生成器；可变日志在包外。真实 AIBrain/ModelRouter 受控 provider/runner/进程/数据库链路用两套包名和操作名验证，不依赖主体里的软件名称分支。

主要文件：core/ai/observation、plugin_reply_bridge、delivery_receipt_archive，及原 PluginRuntime/Catalog、ConversationStore/Service、ActivityStore/Runtime/OpportunityStore、TaskOrchestrator、AIBrain、记忆归属投影、contracts/tools 和根 CMakeLists.txt。新增 test_external_observation.cpp、test_autonomous_chat.cpp，回归保留在原活动、记忆与任务套件；沿用 runtime schema 3，没有第二消息或预算权威。

验证：完整应用与全部测试目标构建成功；最终 CTest **57/57 组通过**（105.56 秒），工具契约 **94/94**。外部观察 42/42、自主聊天 5/5、活动运行时 24/24、任务编排 36/36、记忆归属 27/27；文档命令也在新建临时日志中实际跑通。真实 RED→GREEN、行为映射和 Wave 8 五项验证见审查与计划文档，无未同步设计偏差。日志：/tmp/task8-final-build.log、/tmp/task8-final-ctest.log、/tmp/task8-contract-final.log。

验收后端为本地 SQLite 受控适配器，身份为本地 pet-account/Alice/Bob；没有提供或连接真实 QQ 后端、账号。以上实现及测试不代表已经登录 QQ，也不替代真实模型在开放聊天中的行为评估。
