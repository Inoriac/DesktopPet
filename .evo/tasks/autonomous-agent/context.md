# 自主 Agent 开发上下文

## 需求来源

- 用户确认人物识别和记忆归属优先，再实现通用自主活动，最后接入 QQ 等外部能力。
- 用户要求“做简单 commit，然后进入开发阶段”；设计提交为 `9694046`。
- 主设计：`/Users/huang/Desktop/DesktopPet/contracts/agent/自主Agent-身份记忆与活动运行时-桌面端系分.md`。
- 实施计划：同目录 `自主Agent-身份记忆与活动运行时-桌面端-impl-plan.md`，唯一进度来源。

## 决策记录

- [code-impl Task 6 锚点复核] 已核对 OpportunityStore/Coordinator 的 epoch、机会/预算事务，ActivityRuntime 的单 worker facade 与原库 modelBudgetHooks、ModelRouter::sendAttempt 真实 provider 边界，以及 AIBrain/config/Launcher/屏幕自动入口。完整构建、54/54 CTest（72.34 秒）、70/70 契约通过；重复完成与三个 CAS 竞争问题均有实际 RED→GREEN，旧用例不放宽。

- [code-impl Task 6 全量回归定位] 实际回归暴露 runtime/身份 CAS 的 deferred 读后写事务与新增后台机会写入竞争，单跑重复复现 database is locked；主设计 §1.3 明确此类写事务先 BEGIN IMMEDIATE，保留原 CAS 与回滚。ModelRouter 每次 provider attempt 单独使用一次业务完成门，重复回执仍幂等结算并允许未知用量补实，不能再次完成或触发 fallback。

- [code-impl Task 6 宿主换代] 主设计 §3.7 明确每个 ActivityRuntime/Coordinator 的 hostEpoch 独立于进程 clockSession；初始化原子绑定新 epoch，重置 epoch 内 hostRevision、撤销旧票据并保留水位/预算。解决同进程对象重建及重启后 GUI 版本归零被永久判旧的问题，同时拒绝旧实例的排队写入。

- [code-impl Task 6 热账本边界] heartbeat 的累计时长采样不能无限膨胀 control_json；主设计 §3.7 已明确滚动区段淘汰、累计基线保留，以及同 UoW 将窗口外已结算模型 attempt 移入 model-attempt:<id> 的 migrations marker。保留原费用/归属与迟到幂等，未结算预留不随租约过期删除；复用 §3.6 已有 marker 机制，不新增 schema 或第二预算权威。

- [code-impl Task 6 sanity check] 已验证 AIBrain::canStartProactiveChat() const、setupTriggerTimers()/scheduleTrigger(const QString&)、AgentScheduler::hasTaskDueBefore(const QDateTime&) const/msToNextDue() const、ActivityRuntime::isActive()/taskSnapshot()、AgentRuntimeServices::sleepCycleCoordinator() 与 ModelRouter::completeAsync/completeStreamAsync。模型底层重试已由 ModelRoleRegistry 归零；正式睡眠原先没有 active accessor，不能用 capability availability 替代。
- [code-impl Task 6 设计补全] 主设计 v1.6 同步 §1.5/§3.7/§4.1：宿主 readiness 及水位、同 worker 的机会 CAS 租约、默认关闭的 aiSettings.autonomy 和 Launcher 开关、无 consumer 时只做本地检查、每次 provider send 的异步预算预留/原库结算、自主属性与共享 token/预估费用和 active 小时上限。存储错误零发送，重启不清预算或用户暂停，sourceCursors 合并保留。属于已授权自主调度的工程细化，持续推进授权适用。

- [持续开发授权 2026-10-16] 用户明确要求“你看着往下做吧，每做一个阶段就 commit 一下”。每阶段验证、审查后提交并继续下一阶段，不重复请求阶段确认；不推送远端。
- [命令测试基线修复] QProcess 启动本地编译 fixture 时为 CrashExit，AMFI 日志报告 CMS 签名问题；系统签名 printf/pwd 正常。macOS 测试使用它们验证参数、输出和真实工作目录，其他平台使用独立 helper；增加正常非零退出/异常终止测试。工具新增 exit_status，不改白名单或系统安全设置。全量 CTest 48/48 通过；这不证明本机可执行未具备 CMS 签名的自编译程序。

- [开发授权] 已确认顺序并明确要求进入开发，沿用此授权；每个切片仍执行依赖检查、TDD、审查和 Wave 验证。
- [测试布局] 根目录 `tests/test_*.cpp` + Qt Test；由根 `CMakeLists.txt` 注册，不新增 `tests/CMakeLists.txt`。
- [构建环境] 本机有 Qt 6.8.3：`/Users/huang/Qt/6.8.3/macos`；CMake：`/Users/huang/.qt-build-env/bin/cmake`；已有 `build/` 为本仓库 Debug 配置。测试使用临时库，不操作用户数据库。GUI 与真实 QQ 能力不从单测推定。
- [范围] Task 1 不提前实现多人记忆、会话路由或真实 QQ；可信本地入口兼容与外部身份错误归属防护属于本切片。所有新业务均保持通用 provider/namespace，无 QQ 专用核心分支。
- [code-impl Task 1 设计补全] 按 evoloop-gen-tech-design 补全模式同步 §3.2/§4.1/§5.3：人物 revision CAS、匹配 IdentityBindingAccepted 事件、IdentityBound 同事务 outbox、明确 OriginKind 与历史 source 限定。属于已确认身份对应设计的工程细化，未新增产品范围。
- [code-impl 基线] 修改实现前，现有已构建 CTest 47 组中 45 组通过；FileWebToolTests 的自调用命令退出码 9、PromptRendererTests 的旧默认性格字符串断言失败。此基线来自现有 build 二进制，最终需重新构建再复核，不能直接当作本轮回归。
- [code-impl Task 1 sanity check] §3.2 实现锚点已与源码签名核实一致：`RelationshipService::applyEvidence(const QString&, const RelationshipEvidence&)`、`SqliteIdentityRepository::currentRelationship(const QString&, const QString&) const`、`AgentRuntimeServices::captureSnapshot(const QString&, const QString&) const`、`AgentRuntimeServices::chatPreparationRuntimeMetadata() const`、`SqliteEventRepository::open(const QString&)`、`SqliteRuntimeUnitOfWork::begin(const QString&)`、`EventOutbox::enqueue(RuntimeUnitOfWork&, const EventDraft&)`。
- [code-impl Task 1 持久化边界核对] TaskStore::save 负责构建经历 outbox；仅改 TaskRecord/adapter 会漏 origin。已同步主设计 §3.2 和实施文件清单，包含 task_store.cpp。该调整只保持来源，不改变旧任务生命周期。
- [code-impl Task 1 TDD] 目录测试先因缺 person_directory.h 编译失败；runtime 新场景产生 10 处实际行为失败（空对象默认 owner、外部反馈进入人格证据、未知人物产生关系）；聊天外部/内部空对象失败；任务重启来源丢失失败。对应实现后均变为通过，保留在相应行为测试中。
- [code-impl 验证维护] 默认人格已包含七项 trait，PromptRendererTests 仍断言仅有三项的旧完整句子。按现有产品默认值修正断言，检查七项描述均存在，不依赖排序；不改人格生产逻辑。
- [code-impl Task 1 Wave 回归] 全量编译暴露 ModuleConnectivityTests 的旧 fixture 依赖隐式 owner 和未登记 guest；测试已显式提供 local_owner，并通过 PersonDirectory 取得外部人物 ID，保留原数值和隔离断言。无需放宽生产实现。

- [code-impl Task 2 sanity check] 已验证 §3.3 现有锚点：MemoryExtractor::extractFromUserInput(input, triggerTag)、MemoryPolicy::stageCandidates(candidates, store)、MemoryStore::persistMutationBatch(batch, writtenIds)、loadDatabaseOnly(errorMessage)、MemoryMetadata::mergeContext/semanticRevision、图传播 mayParticipate 回调及 ChatSideEffectQueue writer。新增归属 DTO、共享 ContextSpec 和来源查询接口按主设计 1.2 实施。

- [code-impl Task 2 TDD 与验收] 原始来源校验、错配、关联读写失败、图/临时记忆范围、维护分组、转述对象证明、模型跨人冲突、恢复顺序均取得实际 RED→GREEN。新增损坏 audience 四场景、writer 原置信度门槛和强化时范围/敏感/过期复核；最终完整构建成功，CTest 49/49 通过。聊天缓存断言等待实际异步提交，保留原行为验证。
- [code-impl Task 2 锚点复核] MemoryAttributionService::ingestMemory/recallForContext/migrateAttribution、ChatSideEffectQueue::persistCandidates/migrateNextBatch、MemoryStore::summaryForContext(ContextSpec)、DaydreamConsolidator::recoverMaintenance 已按主设计接入；正式 staging 在迁移写事务内复查，只有精确维护重试租约不阻塞。
- [code-impl Task 3 sanity check] ProfileChatHistoryStore::load/appendFinal、ChatPreparationExecutor::submit/Worker::prepare、AIBrain::submitUserMessage/ActiveDialogueResponse、ContextAssembler::assemble 签名均存在。额外核对 UI 先异步持久化后提交 AIBrain、ProfileChatHistoryStore 按值复制到 QThreadPool、ContextProjection 实际位于 include/ai_types.h，已按主设计 1.3 补齐接入文件与线程边界。
- [code-impl Task 3 原始证据补全] 主设计 §1.3/§3.3/§3.4 v1.3 已同步：MessageObserved 是会话原始消息；UserMessageReceived 保留为处理日志，宿主填写 original_origin_ids/source_message_ids，聚合输入保留多来源。growth 跳过处理日志，反思 alias 回溯已验证原事件，人格/日记继续本地主人范围。历史导入标记 historical_import，自身消息与导入不重放成长或自主回复；外部 request.skills 不读取缺 audience 的学习文本。属于原始事实唯一性和既有功能兼容的修订，沿用持续开发授权。
- [code-impl Task 3 并发与接收边界] 已同步主设计：schema v2 仅门禁，旧版本在写锁内复核迁移；会话读后写事务先预留 writer。消息/回执/导入以事务提交为成功，event_log 后续投递失败由单个后台低频 timer 重投同一 outbox，不漏输入或重发；记忆仍核验真正已提交原事件。外部上下文逐条保留宿主 speaker/message 标签与独立正文，避免参与人列表无法对应发言。显式 external/internal 旧事件同样不得走主人材料 fallback。
- [code-impl Task 3 锚点复核] 已核对 ConversationStore::acceptMessage/prepareContext/publishResponse/recordDeliveryReceipt 与 ConversationService 异步 facade、AIBrain::submitIncomingMessage、ChatPreparationExecutor 权威历史准备、ContextAssembler::assemble 的 scope 门禁、isOriginalOwnerExperience 及反思多来源回溯。ProfileChatHistoryStore 的生产 runtime 路径由 PetWindow 注入，按值复制的存储不持有跨线程 QSqlDatabase。完整构建覆盖新增共享头依赖，统计测试目标亦需 core include，不移除原测试。

- [code-impl Task 4 sanity check] §3.5 原锚点均存在：ToolRegistry::registerTool/executeTool、TaskOrchestrator::allowedTool/readOnlyTool/sourceToolsForGoal、TaskToolExecutor 工具构造、ToolRuntime::execute、PolicyEngine、McpClient/McpToolAdapter 和 SkillStore。另核实 ai_brain_tasks.cpp 有第二份线程工具名集合，PetWindow 直接注册内置工具，AITool 仅检查必填字段；已回填主设计 1.4 的生产入口与标准 Schema 边界，不能只做独立目录测试。

- [code-impl Task 4 派发取消核对] ReplyDispatch 与通用执行器共享 ToolDispatchToken，异步加载不提前标记 started；真实执行前 begin、取消与销毁在同一锁下阻止尚未派发工作。三条真实 RED（空 token）见 /tmp/task4-reply-cancel-red.log，修复覆盖加载时取消、关闭服务和实际发送后取消仍归档。参数映射使用已解析远端回复 ID，回执正文/时间来自后端。主设计 §3.4/§4.3 与接入指南已同步。

- [code-impl Task 4 锚点复核] PluginRuntime::registerPlugin/loadCapability/invokeCapability/setPluginEnabled、makeReplyDispatcher、ToolRuntime::executeAsync、TaskToolExecutor::execute 与 AIBrain::initializePluginRuntime 的实际签名和生产调用已核对；共享 token 仅在实际发送点 begin。标准 Schema/metadata/host grant/回执映射已回填主设计，未新增软件名称执行分支。最终完整构建、51/51 CTest、70/70 Python 契约通过。

- [code-impl Task 5 sanity check] §3.6 七子节齐全；TaskStore::open/unfinished/find/save、TaskOrchestrator::start/assess/plan/verify、AgencyDecisionService::request、RuntimeUnitOfWork::connectionName/commit、TaskExperienceAdapter 均存在。旧单 unfinished 索引与旧 completed 回调确实不支持挂起集合/迟到结果；现有 TaskStore 同步存储要求将生产 runner 整体迁入 worker，不能在 GUI 等待新存储。

- [code-impl Task 5 worker 准入补全] 主设计 §3.6 明确 TaskDependencies::prepareTool(call, completion)：原活动/context/generation 随 DTO 进入宿主线程，PluginRuntime/PolicyEngine 复核参数与效果，worker 只收不可变准入结果且再次检查实例；实际派发仍检查权限/token。不让 worker 直接读取 GUI registry 或阻塞等待 GUI。旧 TaskStore 写门禁同事务复核 schema，拒绝打开后清理连接；存储读取失败不得伪装为空集合。均为 v1.5 已定线程与只读迁移契约的工程细化，沿用持续开发授权。

- [code-impl Task 5 恢复范围补全] §3.4/§3.6 增加 ConversationStore::currentContext(conversationId) 只读宿主接口，复用 readContext 取得真实人物、来源、audience 和版本；不为单纯恢复创建发送预留。能力验证使用刷新后范围，活动写事务内再次校验会话/身份版本。主设计已同步，属于恢复已保存活动的既定语义。

- [code-impl Task 5 完成事件与重试] §3.6 明确 ActionCompleted 由真实回执与原 accepted tracking(sourceId/remoteJobId) 匹配后同事务写出，确定性 eventId 与原 profile/activity/call 绑定；恢复须验证已提交来源及 resumeRule。只有明确无副作用的 failed/cancelled 写动作允许新宿主重试步骤，unknown/succeeded 与迁移旧签名不可绕过去重。旧主动聊天在 autonomyEnabled 切换前保留，ActivityRuntime 存在本身不代表启用新自主循环。

- [code-impl Task 5 耗时与能力校验] ActivityTransition.activeMs / ResumeActivityRequest.foregroundActiveMs 由 worker 单调计时累计填入（-1 表示未提供），活动事务合并最后一个活跃时段，切换后刷新权威投影；不从模型解析、不重置上限。PluginRuntime 以共享 State factory 交出线程安全校验器，实时读取目录/授权及退役状态，复用原 grant 语义，不跨线程持有 GUI QObject。已回填 §3.6。

- [code-impl Task 5 锚点复核] ActivityStore 的六项核心方法、ActivityRuntime 异步命令/单 worker、TaskOrchestrator::activate/resume、ConversationStore::currentContext、PluginRuntime::makeCapabilityValidator、TaskExperienceAdapter 异步 backend 与 runtime schema 3 已接入生产并核对。真实 runner 恢复重载人物范围，类型化活动经历保留 Participant 且不猜 subject/speaker。最终完整构建、52/52 CTest（69.25 秒）、70/70 工具契约通过。
- [code-impl Task 5 生命周期回归] 全量回归复现 ChatSideEffectQueue::stop 在 worker 销毁窗口 SIGSEGV；共享 mutex 下发布 workerClosed 并排队投递修复，重复 stop 回归通过。迟到回执归档失败也核对原 generation/pendingId/active，不能暂停新活动；Completed 两个入口共用未结算动作门禁。全部保留实际 RED 与最终 GREEN 日志。

## 设计偏差

- [code-impl Task 5 设计补全] 按 evoloop-gen-tech-design 补全模式将六项接口细化写入主设计 v1.5：runtime SQLite 3、按值 ActivityStore 与后台 ActivityRuntime、TaskStore 活动投影/旧写入口门禁、动作准备/派发/独立回执归档、宿主依据与双活动 CAS 恢复、ExperienceReady 独立 UUID 和旧经历只读接管。消费 marker 复用 agent_runtime_migrations，原 taskId 保持经历幂等源。属于已确认 §3.6 的工程落点，不新增活动选择或具体 QQ 后端。用户持续开发授权适用。

- [code-impl Task 4 回执生命周期补全] 主设计 §3.4/§3.5 明确原库回执归档独立于 UI/QObject 生命周期。旧 ReplyCompletion 先排服务事件队列再写库，会在服务释放前后两种时序丢失已发送事实；新增独立 FIFO 后台归档队列，持有按值原 profile/path，仅界面通知依赖 QPointer。沿用现有回执表、事务和幂等规则，不建第二账本。对应两个真实 RED 保留在 `/tmp/task4-receipt-sink-red.log`。

- [code-impl Task 4 设计补全] 主设计 §1.3/§2.3/§3.5/§4.2—4.3 v1.4 采用 jsoncons v0.178.0 离线 2020-12 验证、profile plugins.json 的宿主配置和 grant、JSON Pointer 结果/回复映射、显式 HostToolMetadata 与工厂、三个通用 gateway 和统一异步 ToolRuntime。任务去掉工具名称/目标关键词猜测但保留真实证据门槛。取消与禁用使未派发失效，已派发保留原身份回执；沿用用户持续开发授权，不新增软件专用分支或第二动作账本。


- [code-impl Task 2 设计补全] 主设计 1.2 明确事件预分配 UUID、writer 查询已提交来源、pending 来源、共享 ContextSpec、按上下文缓存与迁移顺序。现有 GUI 先 stage 后异步落库不能直接证明事件已提交；最终人物去重和冲突须移到/复核于 writer。新写先统一带保守归属后再批量迁移，不长时间暂停写入；这是 §3.3/§5.3 的明确修订，不改变产品范围。
- [code-impl Task 2 转述引用补全] 目录存在性无法证明原文对象。主设计新增宿主来源 DTO 的 resolvedPeople，转述对象须来自已提交事件的宿主解析引用，未验证 ID 返回 UNKNOWN_REFERENCE。现有规则不会凭昵称认人，缺引用的候选保持 unresolved；严格第一人称沿用已验证 speaker。属于来源校验的工程细化，沿用用户自主推进授权。
- [code-impl Task 2 恢复顺序补全] 归属迁移会改变语义 revision，必须先恢复原 maintenance/daydream_maintenance 批次，并在正式 Prepared staging 清空前延后迁移；使用原 changeId/hash/CAS，不重新调用模型。正常 bootstrap 沿用既有睡眠恢复顺序，独立 writer 用单个低频计时器等待。已同步主设计 §3.3，正式睡眠仍归协调器决策。
- [code-impl Task 3 设计补全] 主设计 1.3 明确 SQLite 会话权威切换、宿主连接及自身账号核验、单 worker 异步 facade、通用 AIBrain 会话入口和消息派发幂等表。新增 recordDeliveryReceipt 保存已发生的迟到结果；外部工具派发通过宿主异步接口，第 4 阶段通用执行器接入，不在 GUI 同步阻塞或添加软件专用分支。历史空 runtimePath 只保留旧解析兼容，生产明确启用 v2，失败不回退。沿用用户顺序开发授权，不新增产品方向。

无未同步的设计偏差。initialize 显式启动接口、人物 revision CAS、接受事件校验与 TaskStore 来源持久化均已回填主设计。

## 待回溯设计的发现

- [pending，§3.8 启动前核对] 自主内部活动的插件授权范围需要显式落点。现有 grant 的 local_owner 只匹配 LocalOwner，其余匹配非空 conversationId；Internal 且无会话的自主阅读不能借用主人身份。§3.8 必须定义宿主内部活动 grant 范围并保持 autonomous 独立开关，再接自主调用，不能仅把 originKind 改成 LocalOwner 取得权限。

inactive 人物管理、合并与拆分尚无本切片入口，后续增加这些操作时须先定义解析/历史更正规则，不能把预留 schema 当作已完成能力。
