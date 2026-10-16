# 自主 Agent 阶段代码审查

审查范围：PersonDirectory、runtime 身份 schema、来源标记及其聊天/任务/经历传播链路；以主设计 §1、§3.2、§4.1 与身份 DDL 为准。此记录不代表其他七个切片已实现。

## 实现核对

- 身份键包含 profile、provider、namespace、remoteActorId；昵称仅用于展示，同名不会合并。缺稳定 ID 保留 unresolved。
- owner 保留字面 ID；initialize 以新增行回填旧 subject，不改 relationship_state 的行、时间或版本；失败回滚整批。
- 人物与首个账号在同一事务创建。写事务在查询前取得 SQLite 写锁，避免并发读后升级造成重复人物；异常竞争重读精确键。
- 新账号绑定校验已提交且内容匹配的宿主 IdentityBindingAccepted 事件，使用人物版本 CAS；映射、revision 与 IdentityBound outbox 同事务。相同成功请求幂等；冲突不迁移历史。
- metadata 默认 internal/无人物，本地入口显式 local_owner/owner。显式空 snapshot 不回退 owner；外部 owner 账号与主人指导权限分离。
- 成长消费者仅对存在的已解析人物产生关系效果，未知/内部来源不写关系，并推进 checkpoint，避免堵塞后续事件。
- TaskRecord JSON、TaskStore 经历 outbox、TaskExperienceAdapter 连续保留 origin_kind。只有缺字段的已知旧本地路径进入兼容，非法显式来源保持 internal。
- repository、unit of work 和 outbox 沿用现有连接/销毁约定，未增加跨库事务或软件名称分支。

## 审查修复

- 并发测试在所有线程抵达 barrier 后再处理 open 失败，避免测试挂起。
- 默认人格旧提示词测试只识别三个 traits，已改为检查现有七项描述，不修改生产人格逻辑。
- 全量回归发现 ModuleConnectivityTests 的旧 fixture 省略 origin，并使用未登记的 guest。按新契约补真实人物解析和显式来源，保留原关系数值/隔离断言；不放宽生产代码以信任未知来源。

## 覆盖映射

| 契约 | 实际测试 |
| --- | --- |
| 稳定账号、昵称变化、同名隔离、角色/namespace 隔离 | test_person_directory.cpp 的 resolvePerson 系列 |
| 缺稳定 ID、不自动 owner、存储失败与并发 | 同文件 missingStableId、storeFailure、concurrentFirstResolution |
| 多账号、主人外部账号、关系历史不变 | newAccountWithEvidence、trustedLocalOwner |
| 证据校验、旧版本、未知人物、重复请求 | evidenceMustMatchHostAndScope 数据表、staleRevisionOrMissingEvidence、newAccountWithEvidence |
| 绑定 outbox 失败回滚 | outboxFailure_rollsBackMappingAndRevision |
| 旧库幂等迁移与初始化失败 | migration_existingRelationshipSubjects、initialize_failureRollsBackOwner |
| 聊天本地/外部/内部空对象 | test_chat_preparation_executor.cpp 的 chatPreparation_emptySubject_respectsOrigin |
| 关系与人格权限、未知对象、历史来源、消费幂等 | test_agent_runtime_services.cpp 的 processIdentityEvents 系列 |
| 任务重启仍保留 external | 同文件 taskExperience_preservesExternalOriginAcrossRestart |
| 真实聊天联通、旧关系增长与他人隔离 | test_module_connectivity.cpp 的关系/成长测试 |

## 范围与保留项

- inactive 是预留状态；本切片不提供人物失效、合并、拆分管理入口，不能声称这些操作已经实现。
- 多人记忆范围、会话路由、自主活动与 QQ 接入仍归后续切片。
- 原命令测试退出码 9 已定位为本机 AMFI 拦截自编译子程序，QProcess 报 CrashExit。macOS 成功测试改用系统签名 printf/pwd，保留真实调用、含空格参数、输出与工作目录断言；其他平台用独立 helper。工具仅新增 exit_status 与崩溃报错，新增正常非零退出及异常终止测试。未绕过系统安全设置；完整 CTest 48/48 通过。

最终构建与测试结果见同目录阶段实现总结；进度仅记录在 impl-plan。

## §3.3 人物记忆归属、召回与迁移

本节记录 §3.3 的实现核对、修复与验收证据。

- 归属必须从真实已提交来源取得；队列重试保留 eventId 和时间，非输入事件仍正常落库。
- 相同摘要还须校验命题、人物和 audience；重复原始来源不增加独立证据。转述的 speaker 与 subject 分开，不能把“我的朋友”认作主人。
- 旧无 context 的调用是本地主人兼容，不能跳过范围或逐条借用记录 profile 自授权限。
- 临时工作记忆在进入 query.text 前过滤，图的中间节点与最终强化均复核范围。
- 话题缓存按稳定会话/活动与权限分组，不以每轮变化的 conversationRevision 分组。
- Daydream 与 MemoryOrganizeTool 两条合并路径均保留来源；不兼容的原条目不能被错误标记已替代。
- SQLite v1→v2 升级、关联写失败回滚、JSON 往返、稳定 cursor 分批迁移及迟到来源的语义版本校验。

### §3.3 覆盖映射

| 契约 | 实际测试 |
| --- | --- |
| 两人相同偏好隔离、同人冲突、原始来源幂等与累积 | test_memory_attribution.cpp：ingestTwoSpeakersAndOrigins |
| 原始事件未提交、迟到补全、错配、查询失败 | pendingSourceAndMismatch、sourceLookupFailureDoesNotWrite；test_chat_side_effect_queue.cpp：attributedInputVerifiesCommittedEventAndRepairsPending |
| 转述对象与参与人、已有目录人物仍需原文引用 | reportedSubjectAndParticipantsSurviveConsolidation、describedPeopleNeedCommittedReferences |
| 第一人称活动与“我的朋友”等歧义 | ownActivitiesHaveVerifiedSubject、ambiguousFirstPersonDoesNotClaimOwner |
| 可见共享知识、私聊范围、图中间节点及工作记忆 | audienceAndJsonRoundTrip、privateGraphBridgeAndWorkingMemoryAreInvisible；test_chat_preparation_executor.cpp：prepare_externalContextFiltersWorkingMemoryBeforeQuery |
| 会话话题独立、稳定缓存键、旧任务摘要入口 | contextSwitchKeepsTopics、contextKeyUsesStableConversation、summariesRespectCurrentAudience |
| 相同摘要不同命题、巩固不跨人物、迟到来源使旧提案失效 | sameSummaryDifferentFactsRemainSeparate、consolidationRejectsOtherSubjectAndLateOrigin、modelConflictCannotCrossPeople |
| 维护工具按人物分组合并、来源集合与失败原子回滚 | test_memory_strategy.cpp：testMemoryOrganizeRespectsPeopleAndAtomicOrigins |
| SQLite/JSON 归属往返、schema 升级和未来版本拒绝 | audienceAndJsonRoundTrip、schemaUpgradePreservesBodyAndRejectsFutureVersion |
| 关联写失败、读失败、迁移断点与原正文不变 | associationFailureRollsBack、attributionReadFailureIsNotLegacy、migrationIsAtomicAndIdempotent、migrationFailureKeepsCursorAndBody |
| 旧维护批次在迁移前恢复、正式睡眠 Prepared 阻塞迁移 | test_sleep_cycle.cpp：maintenanceReplaysDurableBatchExactlyOnce、migrationWaitsForSleepRecovery |
| 原聊天异步写入和删除、来源 session 保留、工具及屏幕临时记忆 | test_streaming_dialogue.cpp 的既有相关场景 |

本表只记录实现与测试入口对应关系；最终构建、测试结果和审查结论在完成后另行记录。

### §3.3 审查修复与定向验证

- 将最终归属、去重和冲突放在串行 writer 中，原始输入事件先幂等提交；目录存在性不再被当作原文引用证明。
- 同一 pending origin 补全为 committed 时保留记忆 ID 和独立证据数；直接自述的 reported→fact 只在校验来源后发生。
- 修复无 context 调用逐条使用记录 profile 的隐式授权，默认由 store.profileId 提供本地主人兼容；临时记忆和任务摘要在构造模型材料前过滤。
- 补齐 MemoryOrganizeTool 合并入口的人物分组和原子提交，保留 Daydream CAS 对新增来源的保护。
- audience 存储格式损坏时拒绝读取，不回退为 owner_only；corruptAudienceIsNotLegacy 四个数据场景从实际失败转为通过。
- writer 提交强化前复核当前范围、敏感标记和到期时间；reinforcementRechecksAudienceAtWrite 的三个数据场景通过。candidatesKeepOriginalWritePolicy 保留旧置信度与显式请求门槛。
- 抽取维护恢复为共享函数，在归属迁移前重放原持久化批次；正式或新到达的 Prepared 变更阻塞迁移。没有放松 payload hash、changeId 或语义版本校验；计时器只重试恢复检查，不生成模型决定。
- StreamingDialogue 的缓存断言改为等待实际异步提交，仍验证重试不增加来源/提及次数、待删除内容不进入当前提示词、最终删除进入缓存和数据库；未删除功能断言。

定向验证：MemoryAttributionTests、ChatSideEffectQueueTests、SleepCycleTests、StreamingDialogueTests **4/4 通过**，日志 `/tmp/attribution-cleanup-green-ctest.log`；完整构建与 49 组回归仍待本阶段最终记录。代码审查未发现剩余阻塞项。

### §3.3 最终结论

代码审查通过，Wave 2 五项校验通过。最终完整构建成功；CTest **49/49 组通过**（13.50 秒），日志 `/tmp/memory-attribution-final-clean-build.log`、`/tmp/memory-attribution-final-clean-ctest.log`。`git diff --check` 通过。未改变真实 QQ 接入状态。

## §3.4 会话路由审查

本节记录会话路由的实现核对、修复及最终验收。

- 统一会话事实来源、旧历史兼容模式、Store 独立连接和 Service 异步 worker 与主设计一致；生产 UI 明确启用 runtime 存储。
- 原始 MessageObserved 与消息同事务，稳定 ID 区分会话，人物/自身账号核验不依赖正文。
- 新外部输入不进入主人命令/任务快捷入口，过渡期不注入原全量工具；发送经过宿主异步绑定。
- 已修复的审查项：预留到实际派发之间的停止窗口；原始消息水位隔离后到输入；historyLimit 校验；100 条迁移 checkpoint 与冲突 ID 映射；先到自身回显的回复链回填。
- runtime 版本升级同步允许 SleepSessionRepository 使用 v2，不改睡眠表及恢复语义。
- 路由测试覆盖取消派发窗口、历史水位、分批迁移及自身回显合并；生产 AIBrain/历史/来源消费与全量回归通过。
- 已同步并落实主设计：会话处理日志不能重复计入原始证据，聚合与反思需多来源回溯；历史导入不重放已消费成长。外部 skills 同工具一样等待有范围目录。

### §3.4 覆盖映射

| 契约 | 实际测试入口 |
| --- | --- |
| 远端重试、不同群相同局部 ID、sourceEventId 回退 | test_conversation_routing.cpp：acceptMessage_remoteMessageRetry_persistsOnce、acceptMessage_sameLocalMessageIdAcrossGroups_isolated、acceptMessage_sourceEventFallback_isStableAndScoped |
| 宿主连接及 local_owner 核验、自身稳定账号回显 | acceptMessage_unregisteredOrForgedLocalOrigin_rejected、acceptMessage_selfEcho_usesConnectionIdentity |
| 按会话准备历史、人物投影、后到消息水位 | prepareContext_switchConversations_restoresOwnHistory、prepareContext_laterQueuedMessage_isNotPriorHistory |
| 原目标固定、旧 generation 拒绝、真实迟到回执 | publishResponse_newMessageInOtherChat_keepsOriginalTarget、publishResponse_staleGeneration_rejectsSendButArchivesActualReceipt |
| 重复/unknown 不重发、自身回显先到合并 | publishResponse_repeatedAndUnknown_neverResends、recordDeliveryReceipt_selfEchoArrivesFirst_mergesOnce |
| 实际 ToolRuntime 异步派发、预留后取消窗口 | service_dispatchesThroughActualToolRuntime_andArchivesReceipt、service_cancelBetweenReservationAndDispatch_preventsSend |
| 原始 outbox 原子回滚、提交后投递故障恢复 | acceptMessage_outboxFailure_rollsBackMessage、acceptMessage_eventDeliveryFailure_staysAcceptedAndRecovers |
| 每批 100 条迁移与失败 cursor、重复启动 | importLocalHistory_commitsBatchesAndDoesNotReimportCompletedSource |
| 旧历史状态/回复链、按值复制跨线程、SQLite 权威 | test_chat_history.cpp：v2History_importsOnceAndWritesOnlySqliteAcrossThreadCopies |
| AIBrain 外部排队、发送目标与历史不串、桌面不显示外部流 | test_streaming_dialogue.cpp：externalConversation_queuesSeparatelyAndKeepsBoundTargetAndHistory |
| 显式上下文的角色与 audience 校验 | test_model_router.cpp：context_scopeFiltersExternalAndMissingProjections |
| runtime v1→v2 与未来版本拒绝 | test_event_ledger.cpp：runtimeSchema_upgradesVersionOneWithoutDroppingEventsAndRejectsFuture |
| 成长只计原始一次、历史导入不重放 | test_agent_runtime_services.cpp：observedMessage_growthCountsOriginalOnceAndSkipsHistoricalImport |
| 聚合处理/反思多来源、历史及外部人格范围 | test_personality_consolidation.cpp：observedMessagesAndSessionLog_preserveOriginalSetWithoutDuplicateEvidence、explicitExternalAndInternalLegacyEvents_doNotEnterOwnerMaterials |
| 双入口先后及同时到达仅一条消息、一份 origin、一次 revision | test_conversation_routing.cpp：acceptMessage_uiAndPreparationRace_oneCommittedMessageAndOrigin |
| 同群两人历史与聚合当前输入、嵌入伪造 speaker 文本不改变宿主标签、historyLimit 门禁 | prepareContext_groupHistoryAndCurrentInput_preserveSpeakerPerMessage |
| 缺派发器明确失败且无新增发出消息 | service_missingDispatcher_rejectsWithoutSending |
| SQLite 写失败不写旧 JSONL、已迁移不依赖旧源、未迁移读取失败可恢复 | test_chat_history.cpp：historyMigration_v2Failure_neverFallsBackToJsonlWrites、historyMigration_completedSqlite_doesNotRequireLegacyFile、historyMigration_sourceReadFailure_doesNotCompleteEmptyImport |
| 原事件投递晚于处理日志时可回溯，同时不越过 cutoff | test_personality_consolidation.cpp：observedMessagesAndSessionLog_deferredDeliveryPreservesSourcesWithinCutoff |
| 存储等待不阻塞界面事件循环 | test_conversation_routing.cpp：service_storageWait_doesNotBlockUi |

覆盖入口已补齐，代码审查无剩余阻塞项。上述测试均包含在最终 50/50 组通过的完整回归中。

### §3.4 审查修复

- 消息接收、历史写入和回执保存以 runtime 事务提交为成功边界，outbox 投递失败保留待恢复事件；Service 使用单个 1 秒 timer 串行检查，归零停止，不能重收或重发。记忆仍验证 event_log 原始来源，不从待投递状态假定已提交。
- 会话读后写事务先预留 writer；已经 v2 的库只做版本门禁，旧版本在 BEGIN IMMEDIATE 内复查再迁移。普通 loadHistory/validateContext 保持只读事务。
- 成长只消费原始 MessageObserved；处理日志保留多 origin 引用，反思 alias 用事实时间、会话/人物/消息和 cutoff 校验，投递顺序不冒充事实顺序。历史导入、自身回显与显式外部/内部来源排除主人材料。
- 历史读取显式区分坏行警告和真实 I/O 失败；迁移完成重新打开只依赖 SQLite，升级失败不回退 JSONL。双 worker 测试的初始 revision 预期修正为 1 + 一次输入 = 2，消息和事件数量仍严格要求 1。
- 外部提示保留逐条宿主 speaker/message 标识，正文独立编码；工具与无 audience 的旧学习技能不流入外部上下文，发送仍只走宿主异步绑定。
- 全量回归对旧库模拟显式设置 user_version=1，不再把人为删除表的 v2 库当作正常旧库；统计目标补新增共享头所需 core include。保留原历史 ID/版本断言及全部测试目标。

### §3.4 最终结论

代码审查通过，Wave 3 五项校验通过。完整应用/所有测试目标构建成功，最终 CTest **50/50 组通过**（42.64 秒），`git diff --check` 通过。日志 `/tmp/conversation-routing-full-build.log`、`/tmp/conversation-routing-full-ctest.log`；首轮旧迁移 fixture 的 RED 保留为 `/tmp/conversation-routing-full-ctest-legacy-fixture-red.log`，未靠删测试取得通过。受控 ToolRuntime 发送证明宿主异步路由，不代表真实 QQ 已接通。

## §3.5 插件发现、按需加载与通用执行

### 审查基线与分段证据

- 现有 Tool 格式 1 的独立验证为 62/62，通过日志 `/tmp/task4-contract-baseline.json`；jsonschema 4.23.0 仅安装到临时目录。
- 第一批 RED 为缺少 plugin_catalog.h；随后标准 Schema、纯 Skill 摘要/按需正文、包外路径三个行为通过。日志 `/tmp/task4-red-catalog.log`、`/tmp/task4-green-catalog-build.log`、`/tmp/task4-green-catalog-test.log`。
- 第二批 RED 为缺少 plugin_runtime.h；command/MCP/启停首批测试为 7/7（含 Qt init/cleanup），日志 `/tmp/task4-red-execution.log`、`/tmp/task4-green-execution-test.log`。
- 通用 registry/异步接口的 RED 保留在 `/tmp/task4-red-tool-async.log`。这些分段结果不替代本阶段完整构建、回归与生产入口验收。

### 审查要求

- Plugin/Skill ID 与既有 Tool ID 使用一致的末尾约束；必需 vocabulary 使用真实支持集合；配置引用种类、重复项和非法 Schema 不得静默接受。
- 在取得 MCP 实际 Schema 前检查宿主执行绑定；缺必需结果映射时不先执行写操作；输出上限和 secret 脱敏覆盖最终结果、回执和错误路径。
- local_owner grant 只匹配本地主人来源；外部上下文不能凭同名 conversationId 借用本地授权。
- 取消 token 贯穿异步 gateway 与最终派发点；目录变更使未派发请求失效。原调用身份不可由执行器返回值覆盖，重复回调只结算一次。
- 角色切换与旧服务释放后，已派发回执仍归档原库；不能提前结算正在运行的调用或在 GUI 等待完整工具超时。
- 聊天一轮多个工具在中途确认后仍继续剩余调用；只有取消当前生成代次才阻止新派发。

### 修复与覆盖映射

| 计划行为 | 实际验证入口 |
| --- | --- |
| 纯 Skill、纯 Tool、混合插件发现与按需详情 | PluginRuntimeTests：registerPlugin_skillOnly_indexesSummaryAndLoadsOnDemand、registerPlugin_mixedDependenciesAndConflict_loadsOnlySelectedDetails |
| script/CLI/MCP 实际执行、空值/标量与多段响应 | invokeCapability_scriptCliAndControlledMcp_returnsActualOutcome、invokeCapability_outcomesAndPreflightAdapters |
| 启停、旧句柄失效、在途回执保留 | setPluginEnabled_disableThenEnable_blocksNewCallsAndInvalidatesOldCache、setPluginEnabled_inFlightReceiptAndQueuedCalls_keepIdentity |
| 清单、配置、包内 Schema 引用和路径拒绝 | schema_rejectsInvalidMetaschemaVocabularyAndTrailingIdentifier、schema_localReferencesAndConditionalConstraints、registerPlugin_rejectsEscapingDefinition、hostConfiguration_duplicateIdentitiesAndDisabledUpgrade_areAtomic |
| 外部上下文、学习文本范围与 secret 快照 | learnedSkills_localOwnerCanLoad_externalAndInternalCannot、invokeCapability_grantsScopeAndSecretEcho；切换配置后仍脱敏原调用使用的 secret |
| 对象/路径 grant 与 MCP 真实 Schema/协议失败 | invokeCapability_objectAndPathGrants_areCheckedBeforeSpawn、invokeCapability_mcpErrorsAndActualSchema_areNotEmptySuccess |
| 任意注册名称与界面响应 | ToolTests：registryRejectsConflictsAndAsyncFactoryUsesDeclaredMetadata；StreamingDialogueTests：arbitraryWorkerAndMiddleConfirmation_preserveWholeRound |
| 禁用、取消、确认后替换与派发后生命周期 | PluginRuntimeTests：gateway_disableQueuedInvocation_preservesInFlightCall；ToolTests：asyncLifecycle_preservesIdentityAndSettlesAfterFacadeDestruction、pendingConfirmation_cannotExecuteReplacementBinding |
| 实际回复映射 | PluginRuntimeTests：makeReplyDispatcher_usesRemoteReferenceAndActualReceipt，核验嵌套参数、远端引用、后端正文与时间 |
| 关闭服务后原库回执、加载时取消 | ConversationRoutingTests：service_destroyedAfterDispatch_archivesReceiptInOriginalProfile（两种时序）、service_cancelDuringSenderPreparation_preventsNewSend（三种时序） |

- 真正的行为 RED 包括非法 multipleOf=0 未被拒绝、secret 被执行器回显、服务销毁丢回执、派发 token 未贯穿异步准备；分别保留在 /tmp/task4-expanded-red.log、/tmp/task4-receipt-sink-red.log、/tmp/task4-reply-cancel-red.log。
- 全量集成首次暴露旧测试注册缺显式宿主元数据，以及聊天工具 round continuation 清理自身捕获导致的崩溃。保留原断言补注册元数据，生产回调使用局部 continuation 副本释放，恢复 PreparingTool 状态事件；定向回归已通过。Task 测试 fixture 调整析构顺序，先释放会使用其成员回调的 runner。
- 通用主机授权精确绑定 plugin/entry/operation，重复插件和 grant 标识拒绝；更新插件保留原禁用状态。标准 metaschema、包内引用、输出上限、业务错误与结果阶段分别验证，不以空返回、已受理或自然语言成功冒充完成。
- 已核对生产 AIBrain 聊天/任务均使用异步 ToolRuntime，任务规划显式声明来源工具、发现/加载不算业务证据；外部任务没有隐式主人权限。SkillDeleteTool 保持原 L4 拒绝。
- Python 契约验证扩展至 70/70，通过日志 /tmp/task4-final-contract.log；主设计 8 个切片各 7 节、59 处本地链接、接入指南 5 段 JSON 与 4 段 SQL 的两库迁移/重复执行/空库撤销均通过。

### 最终结论

代码审查通过，已确认修复项均闭合，Wave 4 五项校验通过。完整应用及全部测试目标构建成功，最终 CTest **51/51 组通过**（64.02 秒），Python 契约 **70/70 通过**，git diff --check 通过。日志 /tmp/task4-final-full-build.log、/tmp/task4-final-full-ctest.log、/tmp/task4-final-contract.log。最后一轮增加的禁用升级测试按目录契约检查 readiness=disabled 与拒绝加载，不要求隐藏条目；新增学习技能测试覆盖本地主人、external、internal 和另一 profile。

消息发送使用真实 command/受控 MCP 与会话存储的组合验证，生命周期测试不触碰真实账号。本阶段不声明真实 QQ 接通，后续活动状态与自主选择按 §3.6—§3.9 实施。


## §3.6 活动执行、挂起、恢复与经历

### 审查基线

- 基线提交 87b8651，完整构建及 51/51 CTest 通过；本阶段从干净工作树开始。
- 现有 TaskOrchestrator 两项真实 RED：暂停任务后仍拒绝另一个活动；runner 销毁后迟到工具结果仍停留 unknown。日志 /tmp/task5-existing-red.log。
- 主设计 v1.5 已补齐 runtime SQLite 3、ActivityStore/后台 ActivityRuntime、兼容任务投影、动作账本与回执归档、恢复依据和经历 outbox 接管。8 个切片各 7 节及两库 4 段 SQL 的迁移/重复执行/空库撤销已通过。

### 核对重点

- 一个前台与多个挂起集合，状态、预算、动作结果只有一份权威；旧 TaskStore/old outbox 不在 v3 继续写。
- 实际结果先归档原 activity/profile，再考虑是否推进；取消或销毁不丢真实回执，也不触发新步骤。
- 等待消息的锚点必须是同会话已归档的真实发出消息，不能用任意字符串或无归档的 receipt 自证；恢复读取已提交事件和当前上下文。
- 生产 runner/store/timer 属后台串行 worker，GUI 只读快照/发命令，模型/工具 DTO 显式带原活动身份。
- 空查询有真实证据即可结束且不写长期经历；模型失败不冒充检查完成。

### 覆盖映射

以下为已加入代码的验收入口；最终通过结果以本阶段最终构建和 CTest 日志为准。

| 契约 | 实际测试 |
| --- | --- |
| 连续多步占前台、Ready 不抢占、过期 revision 拒绝 | ActivityRuntimeTests：startActivity_multistepRead_holdsForegroundAcrossSteps |
| 真实发出消息锚点、等待释放前台 | transitionActivity_sentReceiptWaiting_releasesForeground |
| 匹配回复恢复聊天、保存并恢复阅读检查点 | resumeActivity_matchingReply_resumesChatThenReadingCheckpoint |
| 重启后未知动作只结算一次、同一成功写动作不重发 | observeAction_verifiedLateReceipt_settlesOnceWithoutResend |
| 用户/重启暂停的恢复权限、累计预算与 schema 3 | resumeActivity_restart_keepsBudgetsAndRequiresValidResumeAuthority |
| 有证据的空查询结束、不把模型失败当作完成 | transitionActivity_emptyCheckCompleted_modelFailureNotCompleted |
| 错 profile/generation、损坏预算、派发存储失败 | actionPreparation_rejectsWrongIdentityBudgetAndGeneration、invalidBudgetAndFailedDispatch_neverInventBudgetOrExecute |
| 旧任务重复签名/预算/调用 ID、重复迁移、旧写连接拒绝 | migration_preservesRepeatedCallsAndBlocksBothLegacyWriteModes |
| 原经历 ID、只读旧 outbox 与消费标记 | migration_experienceUsesOriginalSourceAndReadOnlyLegacyOutbox |
| 终态与 outbox 同事务、失败不发布新版本 | terminalOutboxFailure_rollsBackStateAndCallerRevision |
| worker 实际驱动活动、SQLite 写锁期间 GUI 响应 | asyncRuntime_drivesSelectedActivityAndKeepsGuiResponsive |
| 明确无副作用失败允许新步骤、unknown/succeeded 禁止重发 | knownFailedWriteCanRetryButUnknownOrSuccessCannot |
| accepted 跟踪完成事件与恢复规则 | acceptedTrackedActionWaitsAndResumesOnlyAfterVerifiedCompletion |
| 原执行器多任务兼容、销毁后实际回执归原库 | TaskOrchestratorTests：pausedActivityAllowsAnotherAndKeepsOriginalCheckpoint、lateToolReceiptArchivesAfterRunnerDestroyed |
| 外部经历保留人物/会话/audience | TaskExperienceTests：externalExperience_preservesPeopleConversationAndAudience |
| 更换 runtime 后旧模型队列不得新发 | ModuleConnectivityTests：queuedTaskModelDoesNotCrossRuntimeReplacement |
| 插件实际 data 空查询、非空数据不能由空 receipt 伪装 | ActivityRuntimeTests：transitionActivity_emptyCheckCompleted_modelFailureNotCompleted、emptyCheckRejectsNonemptyCapabilityDataEvenWithEmptyReceipt |
| 非空能力引用的启动/恢复与 live 授权/版本/退役校验 | ActivityRuntimeTests：runtimeUsesLiveCapabilityValidatorOnStartAndResume；PluginRuntimeTests：activityValidatorUsesCurrentGrantsVersionAndRetirementWithoutQObjectAccess |
| 非前台活动不覆盖 GUI 快照、转换/切换保留最后一段累计耗时 | ActivityRuntimeTests：typedTransitionPreservesForegroundSnapshotAndElapsedBudget |
| 本地会话存储失败同样不得使用旧 ContextSpec 回退 | ActivityRuntimeTests：resumePropagatesContextStorageErrorsInsteadOfUsingOwnerFallback |

### 审查修复记录

- callId 按活动命名空间隔离；旧重复查询/重试签名保留各自原调用，迁移后的未知/成功写动作仍受等价签名保护。
- 旧 TaskStore 拒绝 schema 3 时清理连接，每次旧写事务复核版本，防止早已打开的连接越过升级门禁。
- 动作账本投影与预算读取传播错误，不把缺失字段、SQL 失败或部分列表当作空结果；仅在提交后发布 revision/generation。
- 使用真实 ConversationStore 发送归档构造等待锚点；恢复通过只读 currentContext 刷新完整范围，再在事务内复查会话/身份版本。
- AIBrain 通过不可变请求与后台执行器通信；宿主工具准入异步返回真实效果分类，完成回执先独立保存，再检查前台实例是否仍有效。
- 外部经历归属测试取得实际 owner_only 错误后修复；终态 ready 事件与原任务经历源 ID 分开，旧 outbox 由新消费标记接管。
- 保留旧主动聊天模式和节奏含义；新旧循环切换属于 §3.7 的明确自主开关，不能以 ActivityRuntime 对象存在代替启用状态。
- 主设计 §5.3 原句混合了旧写入口与主动循环切换时机，已明确两者：schema 3 关闭旧写，autonomyEnabled 显式启用后才切换通用自主机会；与 §3.7 保持一致。
- 三组定向复核与插件真实数据形状测试通过后，全量回归发现 StreamingDialogueTests 在 ChatSideEffectQueue::stop 的退出竞态触发 SIGSEGV。QPointer 检查后 raw worker 仍可能在 invokeMethod 前被销毁；最小修复用共享 mutex 下的关闭标志与排队投递保护生命周期，保留原持久化语义。崩溃证据与最终修复验证纳入本阶段报告。

最后一批回归包括 TaskOrchestratorTests::resumeRefreshesGroupParticipantsInRunnerAndModelScope、lateReceiptStorageFailureDoesNotPauseReplacementActivity，ActivityRuntimeTests::completionRejectsEveryUnsettledActionInTypedAndCompatibilityPaths，TaskExperienceTests::subjectlessActivityRetainsVerifiedParticipantsAsParticipants 和 ChatSideEffectQueueTests::stopRepeatedlyDuringWorkerClose_isSafe。分别验证实际模型范围、迟到错误隔离、全部未结算状态、memory_people 的 Participant 行与关闭窗口；均有真实失败记录或全量崩溃证据，修复后通过。

### §3.6 最终结论

代码审查通过，已确认问题全部闭合，Wave 5 五项校验通过。最终完整应用与所有测试目标构建成功；CTest **52/52 通过**（69.25 秒），Python 契约 **70/70 通过**，git diff --check 干净。最终日志 /tmp/task5-handoff-build.log、/tmp/task5-handoff-ctest.log、/tmp/task5-final-contract.log；主设计结构、链接和 SQL 演练记录 /tmp/task5-doc-checks.log。

Task 6—8 尚未实施，内部无会话自主 grant 按 context.md 的 §3.8 pending 项处理。当前均为临时数据库和测试后端验证，不声明真实 QQ 可用。

## §3.7 空闲判定与自主机会调度

### 审查范围

- 空闲与运行/暂停/休眠/模式/预算门禁分别表达；权威活动、输入准备及已到期义务不能仅由 m_busy 替代。
- 机会预留、宿主版本及消息水位保存在原 runtime 库，和前台互斥；没有 consumer 时不产生票据或模型请求。
- 模型路由的每次 provider attempt 都先预留、再发送、先结算再重试，选择修复和活动模型共享额度；原 profile 的迟到 usage 不依赖 GUI 存活。
- 自主性由已保存活动意图传递；原用户对话与明确用户任务不受自主小时额度限制。
- 新设置默认关闭，Launcher 保存保留工程参数；开启后原主动聊天/idle/屏幕路径转入通用机会。消息按真实入队时间独立聚合，控制和回执不走聚合窗口。

### 审查反馈与验证入口

| 核对点 | 测试或修复入口 |
| --- | --- |
| 真正空闲与可考虑条件分开 | ActivityOpportunitiesTests::evaluateIdle_onlyExternalWaitOrBackgroundMemory_returnsIdle、evaluateIdle_toolWaitValidationOrReadyObligation_returnsBusy、evaluateIdle_controlsRemainSeparateFromIdleness |
| 单票据及前台互斥 | considerOpportunity_idleEnabledBudgeted_reservesOneTicket |
| 暂停、无事退避与新水位 | considerOpportunity_paused_noModelCall、considerOpportunity_unchangedIdle_backsOffUntilImportantEvent |
| 过期时已有其他义务仍释放预留；配置暂停撤销在途票据 | expiredReservationClearsEvenWhenBusyAndConfigPauseRevokesTicket |
| 不同会话交错、固定最长等待 | considerOpportunity_messageBurst_coalescesOnlySameConversationWithinBound；AIBrain 实际队列接入 nextMessageBatch |
| 没有选择器时零票据/模型，开启后停止旧主动循环 | runtime_noConsumerLocalOnly_thenControlledConsumerReceivesOneTicket、ModuleConnectivityTests::autonomyEnabledRoutesLegacyTriggersWithoutConsumerOrModel |
| 首发、fallback 及发送前存储失败 | modelBudget_eachProviderAttemptReservesAndSettlesBeforeFallback |
| 未知 usage 保守计费与迟到幂等 | modelBudget_unknownUsageConservativeAndLateIdempotent |
| 同一个 token/费用账本约束选择与自主活动 | corruptLedgerFailsClosedAndSharedBudgetBlocksAutonomousActivity |
| 活跃时长累计及小时额度用完后暂停 | activeBudget_runningCountsWaitingDoesNot_preservesRestartBase、activeRuntimePausesAtHourlyLimit、activeBudgetExhaustedBlocksNewAutonomousStartResumeAndAction |
| 时钟变化和共享 JSON 字段保留 | clockJumpAndControlJsonMerge_preserveWindowReservationsAndSourceCursors |
| 重启及同进程重建的宿主换代 | restartHostEpochRejectsRetiredReadinessAndPreservesObservedWatermarks；hostEpoch 与 clockSession 分开 |
| 设置默认关闭、启用、暂停和再次恢复 | tests/test_autonomy_config.py |

父级审查要求修复的具体问题：过期票据遇 busy 返回时未清理、配置暂停未撤销票据、publish 失败后仍考虑机会、QPointer 检查与跨线程投递之间的关闭竞态、持久 hostRevision 与新实例版本归零冲突、旧实例控制命令越过 epoch、热账本随 heartbeat 无限增长、用量相加溢出及 bindHostEpoch 提前更新时钟导致 configure 漏结算旧预留。热账本归档和 hostEpoch 的工程契约已同步主设计与 context.md。

定向 RED/GREEN 记录使用 /tmp/task6-* 日志；阶段最终完整验证及提交以实施计划的 Wave 6 记录为准，不以中间编译或本表代替最终通过结果。

### §3.7 最终回归补充核对

- hostEpoch 绑定前保守结算孤立请求，拒绝旧实例控制：bindHostEpochSettlesOrphanReservationBeforeConfigureAndRejectsOldControls。
- 真实 runtime hooks 下结构修复收费，关闭后迟到回执仍归档：runtimeModelHooks_schemaRepairChargesRealLedgerAndLateReceiptSurvivesRetirement。
- 实际用量溢出不能绕过额度，已归档 attempt 不得复用：realUsageOverflowBlocksNextAttemptAndArchivedAttemptCannotBeReused。
- 发送前取消/Router 退役/排队回调丢弃由 UnsentReservation 保守结算；只在真实调用 client 前标记已发送：cancelledOrRetiredRouterClosesReservedAttemptWithoutProviderSend。
- 活跃小时准入先保存当前单调累计值再检查额度，不使用上一轮 heartbeat 样本。
- 重复 provider 回调只完成一次业务，迟到有效 usage 仍补实；各 fallback/repair 独立门控：modelBudget_duplicateProviderReceiptsSettleWithoutCompletingTwice。
- runtime/身份 CAS 的读后写事务先预留 writer，确定性并发回归：sharedRuntimeTransaction_serializesWritesBeforeReadingCasVersion。

全量回归首次 52/54，流式重复完成崩溃与身份 CAS 数据库锁冲突均保留失败证据；最终构建与全量回归已通过，见本节结论。

- 完整回归进一步复现事件 commitCheckpoint 的同类读后写竞争（internal-empty 第 57 次报 database is locked）；仅把自有 CAS 事务起点改为 BEGIN IMMEDIATE，保留版本检查与错误回传。证据 /tmp/task6-checkpoint-diagnostic-red.log。

### §3.7 最终结论

代码审查通过。预留过期/暂停撤销、宿主换代、未知用量与迟到结算、热账本上限、预发送取消、活跃时长最新样本、重复模型完成、runtime/身份/检查点 CAS 并发问题均闭合。全量 CTest **54/54 通过**（72.34 秒），完整应用和全部测试目标构建通过，工具契约 **70/70**；Wave 6 五项校验和 git diff --check 通过。

最终日志：/tmp/task6-final-verified-build.log、/tmp/task6-final-verified-ctest.log、/tmp/task6-final-contract.log。新机会套件 26/26，运行时服务 57/57，流式对话 79/79；两条旧 CAS 用例各连续 50 次、检查点用例连续 100 次通过。未删除或跳过失败断言。

本阶段提供机会与预算基础设施；没有 consumer 时零模型选择。实际选择器及内部自主授权由 §3.8 接入，受控消息端到端由 §3.9 验证。

## §3.8 自主活动选择与持续意图

### 审查范围与初轮反馈

- 生产 consumer 使用已有 ModelRouter 与真实 attempt hooks；未知来源/权限不能由模型构造，实际采用必须启动原 runner。
- 内部自主 grant 贯穿发现、加载、生命周期校验与实际 invoke；保留用户入口兼容，省略 scope 使用旧 conversationId，显式 scope 只使用已定义值。
- 票据消耗与活动前台写入同事务；用户输入/目录换代/预算不足拒绝旧选择，恢复保持原检查点、人物范围和累计用量。
- 记忆/内心材料带真实来源并受范围过滤；有界数据库读取不在跨库 I/O 期间占 runtime 写锁。空材料仍可用合法 schema 正常休息。
- 限量材料优先可恢复意图，再补近期摘要；原记忆的完整历史来源不删除，但本次提示中的来源数量有界。

初轮审查发现并要求回归：空 references 导致非法 enum=[]；纯读材料准备使用 BEGIN IMMEDIATE 跨库持有 writer；最近完成记录挤掉可恢复活动；单记忆来源数组缺上限；新 scope 的错误类型/未定义别名；实际选择用量超额后的采用准入。后续以实际测试和最终 Wave 7 记录确认闭合，不把这些中间反馈视为通过结论。

### §3.8 行为覆盖映射与修复

| 契约 | 实际验证入口 |
| --- | --- |
| 真实人物/记忆/来源引用，拒绝虚构引用与越界 audience | ActivitySelectorTests::selectionMaterials_realSourcesAudienceAndChangedMeaning、selectActivity_unknownReferenceOrWrongAudience_rejectsWithoutDispatch |
| 当前消息不依赖已形成旧记忆 | selectionMaterials_newCommittedMessageWithoutOldMemory |
| 原子采用选择，失败保留票据并回滚前台 | applySelection_ticketConsumptionAndForegroundAcquisition_atomicRollback |
| 新输入、目录换代和实际模型超额拒绝旧提案 | applySelection_newInputCatalogAndActualUsageInvalidateOldChoice（数据驱动） |
| 原范围、检查点及累计预算恢复，用户/重启暂停不自动恢复 | applySelection_checkpointResume_preservesBudgetAndContext_userAndRestartDenied |
| 同范围旧意图可见、撤销记忆授权禁止恢复旧权限 | selectionMaterials_sameScopeOldIntentVisibleAndRevokedMemoryBlocksResume |
| 近期完成记录不挤掉旧意图 | selectionMaterials_eligibleIntentSurvivesRecentCompletedActivities |
| 空材料可休息，失败有别于 wait，真实修复/fallback 共用两次发送额度 | selectActivity_emptyMaterial_hasValidRestSchema、selectActivity_restProviderFailureAndRepairShareActualAttemptBudget |
| Internal 显式 grant 到真实子进程，owner-only 与 autonomous=false 拒绝 | PluginRuntimeTests::internalActivity_explicitAutonomousGrant_propagatesThroughActualToolExecution |
| 加密内心读取返回真实来源，依赖故障明确不可用 | TaskExperienceTests::unresolvedThought_recentReadRetainsRealSourceAndReportsUnavailable |
| 小幅情绪变化保持活动；重要回复先保存计划与验证结果 | TestTaskOrchestrator::smallEmotionChangeDoesNotReselectAutonomousIntent、safeCheckpointPreservesReturnedPlanAndVerification |
| 异步准入及保存失败均不能越过检查点派发 | importantReplyDuringToolPreparationStopsDispatch、failedCheckpointStopsToolDispatch |
| 实际选择/模型/工具/runner 端到端，不根据包/操作名字分支 | TestModuleConnectivity::aiBrain_consumerThroughRealRouterAndRuntime_startsActualRunner（两套不同包与操作名） |
| 实际空查询更新 lastCheckedAt，不写虚假 ExperienceReady，结束后状态离开 running | 同上真实生产链路，直接检查原库与 AIBrain status |
| 配置失败不假报 ready，恢复后可以重新配置 | aiBrain_configurationFailureReportedAndRecovery_doesNotPretendEnabled |
| memoryAccess 默认关闭、错误值关闭、Launcher 保存保留工程权限 | ActivitySelectorTests::autonomyMemoryAccess_defaultDeniedExplicitAndMalformedFailClosed；tests/test_autonomy_config.py |

父级复核确认：MemoryMetadata::semanticRevision 覆盖同 ID 内容改义；Internal 关系来自真实 cue person 引用，只作投影不添加虚构参与人；准备器用只读 runtime 连接且在另库活动读取前关闭；最终 identity/capabilities 丰富后再次检查 32 KiB 材料上限。材料来源、权限、目录与预算均在采用前复核，票据消耗和前台取得同事务。

重要回复回归取得实际 RED：延迟 prepareTool 期间到达回复，以及 checkpoint UPDATE 失败时，旧实现都会多派发一次工具。修复在异步准入后和真实派发前再次检查；存储失败保留 trigger、停止 runner 并报告失败。/tmp/task7-checkpoint-red.log 记录两项失败，/tmp/task7-checkpoint-green.log 记录 TaskOrchestratorTests 30/30 通过。返回的验证结果先处理；已经有充分完成证据时正常完成，不丢掉它再机械暂停。

真实 memoryAccess 组合另由 selectionMaterials_explicitMemoryAccessHonorsBoundConversation 验证：默认仅可见 Internal，显式宿主授权后可见 owner/绑定 conversation，External 重新准备后排除 owner/private，真实 person 引用不制造参与人。

### §3.8 最终结论

代码审查通过，已发现问题全部闭合。完整应用及全部测试目标构建成功；CTest **55/55 通过**（75.44 秒），选择器 21/21、任务执行器 30/30，工具契约 **70/70**。主设计结构/链接/SQL、测试布局/覆盖/锚点和 git diff --check 通过；Wave 7 五项校验通过。

最终日志 /tmp/task7-final-build.log、/tmp/task7-final-ctest.log、/tmp/task7-final-contract.log。实现报告 /tmp/task7-implementation-report.md。验证采用真实本地插件进程和受控模型 provider；本阶段没有连接真实 QQ，外部观察与真实 runner 等待接入属于 §3.9。

## §3.9 外部观察与 QQ 式自主使用验证

### 审查范围与源码预检

- 对 source.json 校验整个定义、配置引用、包内路径、实际 executor 和代次；不以文件存在代替可启动性。source 可选，主动查询无须声明虚假的后台源。
- JSONL 真实进程支持半行/多行、线长与队列上限、backpressure、断连重放及禁用/退役；ack 只能指向已提交结果。
- 消息、原始 outbox、source marker、cursor 与待处理线索共用原 runtime 库事务。序列为稳定正十进制字符串，opaque cursor 不按字典序比较；低序号迟到不能回退水位。
- poll/push 共用真实消息 ID 与来源；陌生会话/身份、错误形状、status 和模型文本不得伪造消息事实。原始时间未知保持 NULL，receivedAt 不能在记忆中变成 original_at。
- 直接自主发送复用 replyBindings 和原派发账本；已由 ConversationService 准备的发送携带 host-only 引用，不从名称/字符串后缀猜测。真实迟到结果归原库，持久失败保留结果且不重新发送。
- TaskOrchestrator 真实 wait 提案只引用已经提供的步骤，由宿主校验原 outgoing/跟踪回执并填写等待权限。未知写操作、未结算步骤不能被“等待”掩盖。
- 已接收外部线索在忙碌、暂停、outbox 延迟、代次变化后可恢复，Internal tick 不能覆盖上下文；同一消息只能走一次回复路径，自身回显/历史不重复触发成长或回复。
- 最终端到端必须经过真实 AIBrain、ModelRouter 受控 provider、runner、command/source 子进程和数据库，验证等待释放、阅读、真实回复、安全切换/恢复；更换包与操作名称、关闭 source 后仍成立。

预检发现已回填设计：ConversationStore::acceptMessage 原先拥有独立事务，需共用事务步骤才能保证 cursor 原子提交；ReplyReceiptArchive 原先写失败后丢弃已弹出回执，须保留待归档结果；MemoryOriginSource 原先直接把 event_log 时间写入 original_at，须透传 unknown。主设计 v1.8 与 Task 8 文件/测试清单已记录，不通过实施计划隐式扩大契约。

### §3.9 实现期审查发现

- 分群查询只覆盖部分日志，不能以最高 sequence 推进整个账号的 source cursor。主设计 §3.9/§4.4 已补宿主 Source/Query 区分；Query 仍原子归档事实、来源与线索，Source 有序流才推进监听水位。真实两群 query→source 用例验证低序号消息不遗漏。
- 普通线索按会话每次提供至多 8 条，未提供的 marker 留待后续消费。实际等待匹配不能只检查当前排在前面的普通会话；否则忙碌期间另一会话的回复无法请求安全检查点。
- 原始消息时间未知须贯穿事件到记忆归属；重复消息 DTO 使用原记录的历史/时间标记。sourceKey 包含连接与自身身份，改绑账号不能继承旧游标；持久游标结构损坏须报告错误。
- 已发送结果在回执归档成功前不通知 runner；每个失败回执独立计数，不能因另一回执成功而错误解除发送门禁。旧 ConversationService 路径没有原生 reply-to 参数时仍保留宿主源消息引用。
- 真实回执先从原始能力结果提取，公开结果另行脱敏；不能把脱敏后的正文写成远端实际消息。持久化失败保留回执，退役后仍使用原库路径补归档。
- 源的协议错误停止该流；事务失败保留已绑定事实并阻止后续水位越过失败事件。facade 退役后 worker 排空已接收队列，禁止新活动和重连。

### §3.9 行为覆盖映射

| 契约 | 实际验证入口 |
| --- | --- |
| poll/push 原消息幂等、跨群相同远端 ID 隔离 | ExternalObservationTests::ingestObservation_pollAndPushSameMessage_oneMessageAndOrigin、sameRemoteIdAcrossGroups_isolated |
| 消息/outbox/marker/cursor 任一写入失败整笔回滚，低序号失败阻止越过水位 | ingestObservation_messageOutboxCursorFailure_rollsBackAndDoesNotAck（四组故障）、failedPoll_thenSourceReplay_preservesBothFacts |
| 稳定序号、迟到重放、换账号隔离、持久游标损坏拒绝 | ingestObservation_sequenceConflictAndLateReplay_doesNotRollBackCursor、corruptedCursorStorage_failsClosed（四组） |
| 分群查询不推进账号监听水位 | partialConversationQuery_doesNotSkipOtherConversationWhenSourceStarts（实际 query→source 进程） |
| outbox 延迟保留线索，每次 8 条，未提供的继续保留 | outboxDeliveryDelay_preservesCueUntilOriginalEventExists |
| 暂停/忙碌/重启及 Internal tick 保持外部上下文与原事件 | AutonomousChatTests::externalCue_busyPauseRestartAndInternalTick_preservesPendingConversationAndEvents |
| source 启停、崩溃重连、半行/多行/错误/超限及旧 generation | setObservationSource_enableReconnectDisable_preservesCommittedCursor、processCrash_reconnectStartsAtCommittedCursor、partialMultipleLinesBoundsAndRetiredGeneration_noUncommittedAck |
| 已接收事实在 facade 销毁后继续原库归档，退役不发新机会 | failedFirstSourceFact_blocksSecondAndDrainsAfterFacadeDestruction、queryCompletesAfterRetire_archivesWithoutNewObservationNotification |
| source 定义、配置、包路径与 executor 错误拒绝 | sourceContract_validDefinition_isAccepted、sourceContract_invalidDefinitionConfigurationPathAndExecutor_rejectsBeforeStart（七组） |
| 仅 completed 准确映射进入消息，无 source 包可实际主动查询 | pollResult_exactCompletedMappingOnly_normalizesMessages（source-disabled / package-without-sources） |
| 实际发送先归档，标准/服务器规范化/含脱敏值正文不失真，self echo 幂等 | autonomousSend_actualReceiptArchivedBeforeWaiting_keepsOriginalScope（plain / normalized / secret） |
| 插件及会话服务销毁后保留失败回执，兼容无原生 reply-to 参数 | lateReceiptStorageFailure_retainsActualFactAfterPluginDestruction、conversationArchive_storageFailureAndDestruction_retainsReceipt、preparedConversationReply_withoutNativeReplyMapping_sendsOnce |
| 真实 runner 等待只用实际步骤，拒绝伪造字段/外来步骤/过长说明/互斥冲突 | TestTaskOrchestrator::taskRunner_realWaitProposal_usesHostReceiptAnchorAndReleasesForeground、invalidWaitProposalCannotInventHostAnchors |
| 未知写操作不能变成消息/事件等待，也不能自动重发 | restartPreservesSnapshotAndBudgetAndDoesNotReplayAction 中 resolveWaitCondition 两种拒绝断言，以及既有未知动作恢复回归 |
| 普通消息不请求检查点，历史洪流不挤掉真回复 | ordinaryObservedMessageDoesNotMatchAnExternalWait、TestActivityRuntime::pendingReplyEvents_filtersHistoricalFloodBeforeLimit（260 条历史消息） |
| user 等待保留 Event 恢复，自主 ActionCompleted 路由幂等且保留预算 | acceptedTrackedActionWaitsAndResumesOnlyAfterVerifiedCompletion（user / self / event） |
| 未知人物/原始时间保持未知，记忆数据库 original_at 为 NULL | missingTimeOrSpeaker_preservesUnknownIdentityAndTime、MemoryAttributionTests::unknownOriginalTimeRemainsNull |
| 实际 AIBrain→ModelRouter provider→runner→command/source→数据库，等待/阅读/回复/检查点/恢复 | AutonomousChatTests::autonomousChat_checkDiscussWaitReadAndResume_preservesPeopleAndContexts（两套包名和操作名） |
| 可运行样例及配置生成，source 长连接保持 stdin 打开时连续 ack 可处理 | verify_contract.py controlled-chat 组；父级文档 CLI 临时日志烟测 |

定向验证已核实；最终 all-target build/CTest 进一步覆盖补充的回执 outbox 故障和非法等待提案。最终套件为外部观察 42/42、活动运行时 24/24、记忆归属 27/27、任务编排 36/36、自主聊天 5/5；工具契约 94/94。

真实 RED→GREEN 包括未知原始时间被拒绝、source schema 缺失、已 accepted 的步骤不能进入真实等待、普通会话线索挡住等待匹配，以及 Python source 在 stdin 保持打开时因文本缓冲未处理连续 ack。突发流测试曾把累计接收数等同于瞬时队列上限，已按队列容量与拒绝后不 ack 的实际契约修正；未降低 256 队列上限。中间用例的 Result.error() 误用和非 const JSON 下标插入 null 属测试/实现错误，修复后重新验证，不作为业务 RED 或成功证据。


### §3.9 最终结论

代码审查通过，已发现问题均已闭合，无未同步设计偏差。真实事实归档、原始人物/会话范围、单前台、累计预算、未知动作去重和用户恢复权限保持。最终全量构建退出码 0，CTest **57/57 通过**（105.56 秒）；工具契约 **94/94**，未跳过测试。主设计结构/SQL、文件链接、覆盖映射、测试布局及实现锚点检查通过，Wave 8 五项通过。

主 agent 另外实际执行接入文档的 inject/read/send/receipt 和两种配置生成命令，全部使用新的临时消息日志。实现 agent 交还所有权后，最终 CTest 由主 agent 执行；原 all-target 会话退出码已复核为 0。

日志：/tmp/task8-final-build.log、/tmp/task8-final-ctest.log、/tmp/task8-contract-final.log。详细报告：/tmp/task8-implementation-report.md。交付了可复用聊天示例及宿主配置生成器；验证后端为本地 SQLite，未提供或连接真实 QQ 后端/账号，未发送真实外部聊天。
