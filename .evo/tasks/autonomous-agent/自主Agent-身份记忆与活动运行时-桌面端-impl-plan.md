# 自主 Agent 实现计划

系分文档：[自主 Agent 系分](../../../../contracts/agent/自主Agent-身份记忆与活动运行时-桌面端系分.md)。本文件是实施进度的唯一来源；设计文档只维护契约和验收条件。

按已确认顺序分为 8 个 Task / 8 个 Wave，串行推进。用户已授权持续开发，每阶段验证后 commit 并继续，不重复请求确认。

测试布局基准：`core/ai/identity/relationship_service.cpp` → `tests/test_identity_state.cpp`，`core/ai/chat/chat_preparation_executor.cpp` → `tests/test_chat_preparation_executor.cpp`；均由根目录 `CMakeLists.txt` 注册 Qt Test。后续切片在启动前复核文件清单与依赖签名。

## Task 1: 人物识别与外部身份对应

- 关联设计: §3.2 人物识别与外部身份对应
- 状态: [x]
- Wave: 1
- 依赖: 无

文件：
- 实现：`core/ai/identity/person_directory.h/.cpp`、`core/ai/identity/identity_types.h/.cpp`、`core/ai/identity/sqlite_identity_repository.h/.cpp`、`core/ai/event/sqlite_event_repository.cpp`、`core/ai/event/event_schema_registry.cpp`、`core/ai/runtime/runtime_types.h`、`core/ai/runtime/agent_runtime_services.h/.cpp`、`core/ai/runtime/agent_runtime_growth.cpp`、`core/ai/chat/chat_preparation_executor.cpp`。
- 必要入口兼容：`core/ai/ai_brain.cpp`、`core/ai/ai_brain_tasks.cpp`、`core/ai/task/task_experience_adapter.cpp`、`core/ai/task/task_orchestrator.cpp`、`core/ai/task/task_types.h/.cpp`、`core/ai/task/task_store.cpp`。
- 测试：`tests/test_person_directory.cpp`、`tests/test_identity_state.cpp`、`tests/test_agent_runtime_services.cpp`、`tests/test_chat_preparation_executor.cpp`、`tests/test_event_ledger.cpp`、`tests/test_task_experience.cpp`、`tests/test_module_connectivity.cpp`；构建：`CMakeLists.txt`。
- 全量验证维护：`tests/test_prompt_renderer.cpp` 修正旧默认人格断言；`tests/test_file_and_web_tools.cpp`、`tests/fixtures/command_probe.cpp`、`core/ai/tools/file_tools.cpp` 验证真实命令调用并区分正常非零退出与进程崩溃。

测试用例（来自 §3.2.7、§3.2.6 和 §1 约束）：
- resolvePerson_sameAccountRenamed_preservesPersonId（happy path）。
- resolvePerson_sameDisplayName_distinctPeopleAndRelationships。
- resolvePerson_sameAccountDifferentProfile_isolated。
- resolvePerson_missingStableId_unresolvedWithoutSharedPerson。
- resolvePerson_trustedLocalOwner_preservesOwnerHistory（happy path）。
- resolvePerson_concurrentFirstResolution_onePersonAndMapping。
- resolvePerson_storeFailure_noOwnerFallbackOrPartialPerson。
- bindIdentity_newAccountWithEvidence_reusesPersonAndHistory（happy path）。
- bindIdentity_conflictingAccount_preservesOriginalMapping。
- bindIdentity_staleRevisionOrMissingEvidence_rejectsWithoutMutation。
- bindIdentity_mismatchedAcceptedEvent_rejectsWithoutMutation。
- bindIdentity_repeatedSuccessfulRequest_isIdempotent。
- bindIdentity_outboxFailure_rollsBackMappingAndRevision。
- initialize_existingRelationshipSubjects_preservesIdsAndVersions（happy path，含重复启动与错误）。
- chatPreparation_externalEmptySubject_noOwnerRelationship；localOwner_preservesCompatibility。
- processIdentityEvents_externalFeedback_targetsResolvedPerson；unresolved_skipsRelationshipEffects；localOwner_preservesGrowth（happy path）。
- taskExperience_externalOriginAcrossRestart_preservesOriginThroughTaskJsonOutboxAndEvent（happy path）。

Task 1 实现、定向验收与代码审查已完成。覆盖映射见同目录审查记录。

Wave 1 校验（2026-10-16）：
- 文件冲突：通过，单 Task 串行实施；主 agent 仅补设计、记录及独立测试断言。
- 构建/全量测试：完整项目编译通过；最终 CTest **48/48 组通过**。原命令测试失败已定位为本机 AMFI 签名拦截，改用平台适配的真实子程序 fixture，未跳过测试。
- 行为覆盖：本切片每项均有对应测试，包含 initialize/resolvePerson/bindIdentity 的正常路径，以及外部来源、任务重启传播。
- 测试布局：通过，根 tests/test_*.cpp、根 CMakeLists.txt 注册。
- 实现锚点与留痕：通过，已核对实际签名并记录 context.md。

Wave 1 五项校验通过；下一设计项为 Task 2。本次结果不代表后续 Wave 已完成。

## Task 2: 人物记忆归属、召回与迁移

- 关联设计: §3.3 人物记忆归属、召回与迁移
- 状态: [x]
- Wave: 2
- 依赖: Task 1

文件：
- `core/ai/memory/` 新增 memory_attribution_types 与 memory_attribution_service；修改抽取、策略、存储、SQLite repository、metadata、召回、活跃池及巩固。
- `core/ai/tools/memory_tools.cpp` 现有维护工具的重复合并同样遵守人物/范围；回归在 `tests/test_memory_strategy.cpp`。
- 全量回归兼容：`tests/test_streaming_dialogue.cpp`、`tests/test_sleep_cycle.cpp`，验证异步提交、本地入口与旧巩固检查点恢复。
- `core/ai/context/context_spec.h`、`core/ai/reflection/memory_consolidation_service.*`；`core/ai/ai_brain.h` 与 ai_brain*.cpp 的必要记忆入口；`core/ai/chat/chat_preparation_types.h`、`chat_preparation_executor.cpp`、`chat_side_effect_queue.h/.cpp`；`core/ai/task/task_experience_adapter.cpp`；事件仓库/校验器如需补来源查询。
- 测试：新增 `tests/test_memory_attribution.cpp`；现有 `tests/test_memory_strategy.cpp`、`tests/test_memory_recall.cpp`、`tests/test_memory_recall_phase2.cpp`、`tests/test_chat_preparation_executor.cpp`、`tests/test_chat_side_effect_queue.cpp`、`tests/test_memory_recall_phase3.cpp`（巩固回归位于 memory_strategy） 、`tests/test_task_experience.cpp`、`tests/test_module_connectivity.cpp`，按实际入口选取；构建：根 `CMakeLists.txt`。

测试用例：
- ingestMemory_twoSpeakersSamePreference_separateAttribution（happy path）。
- ingestMemory_preferenceChanged_onlySamePersonConflicts。
- ingestMemory_reportedClaim_preservesSpeakerAndSubjectAfterConsolidation。
- recallForContext_allowedSharedKnowledge_recalls（happy path）。
- recallForContext_privateConversation_excludesOtherConversationsAndGraphPaths。
- migrateAttribution_repeatedBatch_preservesIdsSourcesAndProgress（happy path）。
- migrateAttribution_ambiguousLegacyText_remainsUnresolvedAndOwnerOnly。
- ingestMemory_sameOriginIdempotent_distinctOriginsAccumulate。
- ingestMemory_pendingSourceLaterCommitted_resolvesWithoutDuplicateEvidence。
- ingestMemory_mismatchedSourceOrStoreFailure_doesNotInventPerson。
- memoryAttribution_sqliteAndJsonRoundTrip_preservesPeopleAudienceOrigins。
- memoryAttribution_associationFailure_rollsBackBody。
- recallForContext_contextSwitch_restoresOwnTopicCandidates。
- recallForContext_hiddenNodes_notTraversedReinforcedOrCached。
- consolidate_incompatibleSubjectsOrAudience_retainsSeparateMemories。
- consolidate_lateOrigin_retainedOrRejectsStalePlan。
- migrateAttribution_batchFailure_keepsCursorAndOriginalMemoryFields。
- chatMemory_committedInputEvent_isActualOrigin；failedEvent_remainsUnresolved。

具体接入与迁移边界以主设计 1.2 修订为准。

Wave 2 校验（2026-10-16）：
- 文件冲突：通过，单 Task 串行实施，主 agent 维护设计与记录；实现 agent 已交回构建，无并发写入。
- 构建/全量测试：完整应用及所有测试目标构建成功；最终 CTest **49/49 组通过**，13.50 秒。聊天与睡眠旧回归均已修复，无跳过。
- 行为覆盖：上述场景与测试映射见同目录审查记录；追加真实来源引用证明、损坏 audience 拒绝读取、原写入策略、旧巩固检查点先恢复等回归均通过。
- 测试布局：通过，新增 tests/test_memory_attribution.cpp 由根 CMakeLists.txt 注册。
- 实现锚点与留痕：通过，已核对归属服务、writer、范围召回、临时记忆、任务摘要和恢复路径，context.md 有 sanity check 与修订记录。

Wave 2 五项校验通过。下一设计项为 Task 3；真实 QQ 尚未接入。

## Task 3: 会话输入、上下文准备与回复定位

- 关联设计: §3.4 会话输入、上下文准备与回复定位
- 状态: [x]
- Wave: 3
- 依赖: Task 1、Task 2

文件：`core/ai/chat/` 新增会话目录/服务、历史存储、准备 DTO/executor、写队列来源适配；`core/ai/context/context_assembler.h/.cpp`、`context_spec.h`、`include/ai_types.h`；`core/ai/ai_brain.h` 及聊天实现；生产历史接入 `ui/petwindow.cpp`、`ui/chat_conversation_model.h/.cpp`；runtime 表/MessageObserved 接入：`core/ai/event/sqlite_event_repository.cpp`、`core/ai/event/event_schema_registry.cpp`、`core/ai/runtime/agent_runtime_services.*`、`runtime_types.h`；同库版本兼容 `core/ai/reflection/sleep_session_repository.cpp`；`tests/test_chat_preparation_executor.cpp`、`tests/test_chat_history.cpp`、`tests/test_chat_conversation_model.cpp`、`tests/test_streaming_dialogue.cpp`、`tests/test_model_router.cpp`（已有 ContextAssembler 测试）、`tests/test_event_ledger.cpp`（runtime schema 版本门禁）、`tests/test_sleep_cycle.cpp`（共用库兼容）、新增 `tests/test_conversation_routing.cpp`；`CMakeLists.txt`。

测试用例：
- acceptMessage_remoteMessageRetry_persistsOnce（happy path）。
- acceptMessage_sameLocalMessageIdAcrossGroups_isolated。
- prepareContext_switchConversations_restoresOwnHistoryAndTopic（happy path）。
- publishResponse_newMessageInOtherChat_keepsOriginalTarget（happy path）。
- publishResponse_staleGeneration_rejectsSendButArchivesActualReceipt。
- recordDeliveryReceipt_lateOrDuplicateReceipt_mergesSelfEchoOnce（happy path）。
- historyMigration_existingJsonl_importsOnceAndKeepsReplyChain（happy path）。
- historyMigration_v2Failure_neverFallsBackToJsonlWrites。
- acceptMessage_uiAndPreparationRace_oneCommittedMessageAndOrigin。
- acceptMessage_claimedSelfOrOwner_requiresHostIdentityAndLocalBinding。
- publishResponse_missingDispatcherAndRepeatedDispatch_doesNotSend。
- contextAssembler_externalContext_requiresRoleAndAudience。
- conversationService_asyncStorageAndDispatcher_doNotBlockUi。
- aiBrain_externalConversationQueue_keepsOriginalTargetAndSeparateHistory（happy path）。
- sourceEvidence_observationAndProcessingLog_countOnce（happy path，重试幂等）。
- sourceEvidence_aggregatedInputAndReflection_preserveAllOriginals。
- sourceEvidence_externalFeedback_doesNotBecomeOwnerGuidance。
- historyMigration_importAndSelfEcho_doNotReplayGrowthOrPersonality。
- acceptMessage_outboxDeliveryFailure_keepsAcceptedInputAndRetriesSameEvent。
- prepareContext_groupHistoryAndCurrentInput_preserveSpeakerPerMessage。

来源消费接入文件（主设计 §3.4 / v1.3）：`core/ai/runtime/agent_runtime_growth.cpp`、`core/ai/runtime/agent_runtime_personality.cpp`、`core/ai/reflection/personality_consolidation_stage.h/.cpp`、`core/ai/event/event_types.h/.cpp`；相应回归在 `tests/test_agent_runtime_services.cpp`、`tests/test_personality_consolidation.cpp`、`tests/test_sleep_cycle.cpp` 及既有集成测试。原始事实、会话日志和派生反思的计数边界遵守主设计，不新增成长规则。

全量兼容核对：`tests/test_person_directory.cpp` 的旧库模拟在删除旧版尚无的身份表后须显式设置 user_version=1；v2 正常打开不重复 DDL，既有 ID/版本保留断言不变。统计测试目标按新增共享类型补 core include 路径。

Wave 3 校验（2026-10-16）：
- 文件冲突：通过，单 Task 实现 agent 串行修改源代码，主 agent 维护设计与审查；最终构建权已交回。
- 构建/全量测试：完整应用及所有测试目标构建成功，最终 CTest **50/50 组通过**，42.64 秒。原迁移 fixture 已明确旧版本；统计目标保留并通过，未跳过失败测试。
- 行为覆盖：各项与真实方法的映射见审查记录；新增双 worker 同 ID、消息与回执幂等、取消窗口、群聊逐条身份、来源计数、历史 I/O 与范围隔离均通过。
- 测试布局：通过，新增 tests/test_conversation_routing.cpp 由根 CMakeLists.txt 注册。
- 实现锚点与留痕：通过，会话 Store/Service、生产 AIBrain/UI、准备器、范围投影及原事件消费者已与主设计核对，context.md 有记录。主设计四段 SQL 的两库迁移/重复迁移/空库撤销和全部本地文件链接再次验证通过。

Wave 3 五项校验通过。下一设计项为 Task 4；自主活动选择与真实 QQ 仍不属于此阶段交付能力。

## Task 4: 插件发现、按需加载与通用执行

- 关联设计: §3.5 插件发现、按需加载与通用执行
- 状态: [x]
- Wave: 4
- 依赖: Task 3

文件：`core/ai/plugin/{capability_types,plugin_catalog,plugin_runtime,schema_validator,capability_executor,learned_skill_adapter}.h/.cpp`、`core/ai/ai_tool.h`、`core/ai/tool_registry.h/.cpp`、`core/ai/tools/runtime/{tool_runtime,task_tool_executor,tool_policy}.h/.cpp`、`core/ai/task/{task_orchestrator,task_types}.h/.cpp`（按实际已有文件）、`core/ai/mcp/mcp_client.h/.cpp`、`core/ai/mcp/mcp_tool_adapter.h/.cpp`、`core/ai/skill/skill_store.h/.cpp`；`core/ai/ai_brain_plugins.cpp` 及 `ai_brain.h/.cpp`、`ai_brain_loop.cpp`、`ai_brain_tasks.cpp`、`ui/petwindow_ai.cpp` 必要生产入口；`third_party/jsoncons`、`contracts/tools/` 清单校验/资源及正反示例；`tests/test_tool_registry.cpp`、`tests/test_task_orchestrator.cpp`、新增 `tests/test_plugin_runtime.cpp` 及 `tests/fixtures/plugin_*`；根 `CMakeLists.txt` 注册资源与测试。

测试用例：
- registerPlugin_skillToolAndMixedPackages_indexesSummaries（happy path）。
- loadCapability_skillThenTool_loadsOnlyRequestedDetails（happy path）。
- invokeCapability_scriptCliAndControlledMcp_returnsActualOutcome（happy path）。
- invokeCapability_emptyAcceptedFailedCompleted_distinguishesOutcomes。
- setPluginEnabled_disableThenEnable_blocksNewCallsAndInvalidatesOldCache（happy path）。
- setPluginEnabled_inFlightReceipt_keepsOriginalCallIdentity。
- registerPlugin_invalidSchemaReferencesAndPackagePaths_rejectsWithoutGuessing。
- loadCapability_scopedCatalogAndDetails_preservesHostSecretsAndLearnedSkillScope。
- invokeCapability_hostGrantAndObjectPathScope_rejectsBeforeDispatch。
- invokeCapability_mcpActualSchemaAndMultipartErrors_keepsProtocolAndBusinessMeaning。
- executeAsync_arbitraryRegisteredNameAndSlowCommand_usesMetadataAndKeepsUiResponsive。
- executeAsync_disabledQueuedCall_rejectsAndDispatchedCallKeepsReceipt。
- makeReplyDispatcher_controlledConversation_returnsActualScopedReceipt（happy path）。
- makeReplyDispatcher_profileSwitchOrServiceShutdown_archivesOriginalReceipt。

回归兼容：`tests/test_streaming_dialogue.cpp`、`tests/test_schedule_connectivity.cpp`、`tests/test_file_and_web_tools.cpp`、既有调度/模块联通测试显式注册宿主元数据，保留原策略和断言；新增中途确认继续执行与 queued/在途生命周期场景。

生产生命周期补充：`core/ai/chat/conversation_types.h`、`conversation_service.h/.cpp` 及必要的 runtime shutdown 接口参与已派发回执归档；服务或 profile 释放后不得仅因前台接收对象失效就丢失原库回执。

Wave 4 校验（2026-10-16）：
- 文件冲突：通过，单 Task 实现 agent 串行修改，父级仅负责明确划分的会话派发/回执补充与文档；已交回构建所有权，全部源码停止编辑。
- 构建/全量测试：完整应用与所有测试目标构建成功，最终 CTest **51/51 组通过**，64.02 秒；Python 契约 **70/70 通过**，无跳过。日志 /tmp/task4-final-full-build.log、/tmp/task4-final-full-ctest.log、/tmp/task4-final-contract.log。
- 行为覆盖：14 项计划行为均映射到测试，详见审查记录；学习文本范围、异步取消/销毁、任意名称工具、中途确认与实际消息回执均验证。
- 测试布局：通过，新增 tests/test_plugin_runtime.cpp 和 tests/fixtures/plugin_probe.py，根 CMakeLists.txt 注册；标准库 vendoring 独立放 third_party/jsoncons 并保留 LICENSE。
- 实现锚点与留痕：通过，目录/运行时、ToolRegistry/PolicyEngine/TaskToolExecutor、生产 AIBrain、MCP、ReplyDispatcher 与主设计一致；context.md 有 sanity check 与修订记录。文档 8 切片/8 Task、各 7 子节、59 本地链接、指南 5 段 JSON 及 4 段 SQL 演练通过。

Wave 4 五项校验通过。下一设计项为 Task 5；当前验证没有连接真实 QQ。

## Task 5: 活动执行、挂起、恢复与经历

- 关联设计: §3.6 活动执行、挂起、恢复与经历
- 状态: [x]
- Wave: 5
- 依赖: Task 3、Task 4

文件：新增 `core/ai/activity/` 下类型、存储、运行时与串行 worker；修改 `core/ai/task/{task_store,task_types,task_orchestrator,task_experience_adapter}.*`，按实际接口调整 `agency_decision_service.*`；`core/ai/ai_brain.h/.cpp`、`ai_brain_tasks.cpp` 及必要活动接入文件；`core/ai/runtime/agent_runtime_services.*`、事件仓库/校验器/事件与 outbox 接口、`core/ai/reflection/sleep_session_repository.cpp`；如 DTO/派发绑定需要，调整 `core/ai/tools/runtime/` 与 `core/ai/plugin/`。测试：`tests/test_task_orchestrator.cpp`、`tests/test_task_experience.cpp`、新增 `tests/test_activity_runtime.cpp`；生产和版本回归在现有 streaming_dialogue、event_ledger、agent_runtime_services、sleep_cycle 等对应套件，根 `CMakeLists.txt` 注册。父级维护主设计/过程文档，单实现 agent 独占 build。

测试用例：
- startActivity_multistepRead_holdsForegroundAcrossSteps（happy path）。
- transitionActivity_sentReceiptWaiting_releasesForeground（happy path）。
- resumeActivity_matchingReply_resumesChatThenReadingCheckpoint（happy path）。
- observeAction_verifiedLateReceipt_settlesOnceWithoutResend（happy path）。
- resumeActivity_restart_keepsBudgetsAndRequiresValidResumeAuthority。
- transitionActivity_emptyCheckCompleted_modelFailureNotCompleted。
- prepareAction_sameLogicalWriteStep_returnsOriginalCallAndRejectsStaleOrOverBudget（happy path 与异常）。
- markActionDispatched_storeFailure_doesNotInvokeExecutor。
- migration_runtimeV2ToV3_preservesTaskBudgetUnknownActionsAndRejectsLegacyWrites（happy path，重复迁移）。
- experience_emptyCheckSkipped_meaningfulExternalFactPreservesContextAndConsumesOnce（happy path，失败重试）。
- activityRuntime_slowStorageAndTool_keepsGuiResponsive。
- pausedActivityAllowsAnotherAndKeepsOriginalCheckpoint；lateToolReceiptArchivesAfterRunnerDestroyed（原生产执行器回归）。

具体工程契约以主设计 v1.5 §3.6 为准；旧同步 runner 在后台 owner 线程使用，AIBrain 使用已发布快照与异步命令。

必要集成补充：ConversationStore::currentContext 供恢复刷新范围；PluginRuntime 的共享状态 validator 及 `tests/test_plugin_runtime.cpp` 验证动态授权。全量回归定位的 `ChatSideEffectQueue::stop` 退出竞态纳入最小修复，必要变更位于 `core/ai/chat/chat_side_effect_queue.cpp` 及其既有测试套件。保留旧主动聊天行为，§3.7 使用明确自主开关切换。

Wave 5 校验（2026-10-16）：
- 文件冲突：通过，单实现 agent 串行负责源码和 build，父级维护设计与审查；最终源码/build 所有权已交回，无并发修改。
- 构建/全量测试：完整应用及所有测试目标成功，最终 CTest **52/52 通过**，69.25 秒；Python 工具契约 **70/70 通过**。日志 /tmp/task5-handoff-build.log、/tmp/task5-handoff-ctest.log、/tmp/task5-final-contract.log。
- 行为覆盖：全部计划行为映射到实际测试，见审查记录；补充实际插件数据、双活动切换、旧写门禁、完整参与人、真实恢复范围、迟到错误和退出竞态，均通过。未以删减断言取得通过。
- 测试布局：通过，新增 tests/test_activity_runtime.cpp，由根 CMakeLists.txt 注册；其他回归放既有对应套件。
- 实现锚点与留痕：通过，活动 Store/worker、兼容 runner、生产 AIBrain、共享状态能力校验、会话当前上下文和经历消费者均核对；context.md 已留痕。8 切片各 7 节、8 Task、本地链接与两库 SQL 演练通过，git diff --check 干净。

Wave 5 五项校验通过。下一设计项为 Task 6；本阶段验证使用测试后端，不代表真实 QQ 已接通。

## Task 6: 空闲判定与自主机会调度

- 关联设计: §3.7 空闲判定与自主机会调度
- 状态: [x]
- Wave: 6
- 依赖: Task 5

文件：新增通用机会类型、Store、Coordinator（复用 `core/ai/activity/` 和 ActivityRuntime worker）；调整 `activity_store/runtime.*`、`task_types/orchestrator.*` 的宿主自主属性与累计预算接入，`core/ai/ai_brain.h/.cpp`、`ai_brain_loop.cpp`、`ai_brain_tasks.cpp`、`core/ai/scheduler/`、`core/ai/runtime/` 与 `core/ai/reflection/sleep_cycle_coordinator.*`。模型预算 hooks 位于 `include/ai_types.h`、`core/ai/model/model_router.*`；全量回归修复涉及 `core/ai/event/runtime_unit_of_work.cpp`、`sqlite_event_repository.cpp`、`core/ai/identity/sqlite_identity_repository.cpp` 的 CAS 写事务；配置位于 `core/configLoader/config_manager.*`、`config/default_common_config.example.json`、`launcher/app_state.py`、`launcher/config_loader.py`、`launcher/pages/ai_page.py`，必要屏幕主动聊天入口 `ui/petwindow_screen_chat.cpp`。测试放既有 model_router、config_manager、module_connectivity、streaming_dialogue、agent_scheduler、sleep_cycle 等对应套件，新增 `tests/test_activity_opportunities.cpp` 和既有布局下 launcher 配置测试；根 `CMakeLists.txt` 注册。父级维护设计/过程文档，单实现 agent 独占源码和 build。

测试用例：
- evaluateIdle_onlyExternalWaitOrBackgroundMemory_returnsIdle（happy path）。
- evaluateIdle_toolWaitValidationOrReadyObligation_returnsBusy。
- considerOpportunity_idleEnabledBudgeted_reservesOneTicket（happy path）。
- considerOpportunity_paused_noModelCall。
- considerOpportunity_messageBurst_coalescesOnlySameConversationWithinBound。
- considerOpportunity_unchangedIdle_backsOffUntilImportantEvent。
- finishOpportunity_waitCancelExpiryAndRestart_preservesChargedUsageAndRejectsStale。
- modelBudget_eachSendFallbackAndRepairCharged_unknownUsageConservativeAndLateIdempotent（happy path 与异常）。
- modelBudget_storageFailure_neverCallsProvider；sharedBudget_selectionAndAutonomousActivityCounts_userTaskUnaffected。
- activeBudget_runningCountsWaitingDoesNot_pauseAtHourlyLimit。
- autonomyConfig_defaultOffOptInAndPause_roundTripsWithoutOverwritingOtherFields（happy path）。
- runtime_noConsumerLocalOnly_enabledDoesNotDoubleRunLegacyProactive。
- clockJumpAndControlJsonMerge_preserveWindowReservationsAndSourceCursors。

Task 6 工程约定以主设计 v1.6 的 §1.5、§3.7 与 §4.1 为准；无 consumer 时只本地检查，Task 7 才安装真实选择器。

Wave 6 校验（2026-10-17）：
- 文件冲突：通过，单 Task 串行实施；实现 agent 已交回源码/build，主 agent 仅维护设计与审查。
- 构建/全量测试：完整应用及全部测试目标构建成功；最终 CTest **54/54 组通过**（72.34 秒）。新机会套件 26/26，运行时服务 57/57，流式对话 79/79。工具契约 **70/70**。
- 行为覆盖：计划各项均有对应测试，见审查记录；附加真实 fallback/修复计费、迟到结算、重启水位、单调时钟、取消窗口与预算溢出回归。两条原 CAS 竞争各 50 次、检查点竞争 100 次均通过。
- 测试布局：通过，新增 tests/test_activity_opportunities.cpp 与 tests/test_autonomy_config.py 由根 CMakeLists.txt 注册。
- 实现锚点与留痕：通过，AIBrain、ActivityRuntime、OpportunityStore/Coordinator、ModelRouter 实际 attempt、睡眠状态及模型/活动预算接入已核对；context.md 有记录。主设计八切片/七子节、SQL 迁移/回滚和 38 条本地链接检查通过；git diff --check 干净。

最终日志：/tmp/task6-final-verified-build.log、/tmp/task6-final-verified-ctest.log、/tmp/task6-final-contract.log。Wave 6 五项通过；Task 7 才安装真实自主选择器。

## Task 7: 自主活动选择与持续意图

- 关联设计: §3.8 自主活动选择与持续意图
- 状态: [x]
- Wave: 7
- 依赖: Task 1—Task 6

文件：新增 `core/ai/activity/activity_selector.*` 及材料类型/准备模块；调整 `activity_types/store/runtime.*`、`opportunity_types/store/coordinator.*` 的选择权限与原子采用；`core/ai/ai_brain*.cpp/.h` 安装真实 consumer；`core/ai/task/{task_types,task_orchestrator,agency_decision_service}.*` 接续活动与宿主自主属性；`core/ai/plugin/{plugin_runtime,capability_types}.*`、`plugin_gateway.cpp` 及 `core/ai/tools/runtime/`、`ai_tool.h` 的内部范围和调用属性传播。按现有结构调整身份/persona/记忆/私有内心仓库服务的只读投影，必要 `core/ai/runtime/agent_runtime_services.*`；`AutonomyConfig`、`core/configLoader/config_manager.*`、默认配置及 `tests/test_autonomy_config.py` 接入/验证显式 memoryAccess 与保存兼容。新增 `tests/test_activity_selector.cpp`；相关回归沿用 `tests/test_activity_opportunities.cpp`、`test_activity_runtime.cpp`、`test_plugin_runtime.cpp`、`test_module_connectivity.cpp`、`test_task_orchestrator.cpp`、`test_sleep_cycle.cpp`、`test_task_experience.cpp`（现有私有内心服务测试所在套件）。根 `CMakeLists.txt` 注册；主 agent 维护设计与过程文档，实现 agent 独占源码/build。

测试用例（来源主设计 v1.7 §3.8.7 及工程边界）：
- selectActivity_relevantFriendMemory_proposesEvidenceBasedCheck（happy path，真实 person/memory/source 引用）。
- selectActivity_unknownReferenceOrWrongAudience_rejectsWithoutDispatch。
- applySelection_newReadingCapability_startsWithoutSoftwareNameBranch（happy path，两个不同名称包与真实 runner）。
- internalActivity_explicitAutonomousGrant_propagatesThroughActualToolExecution；ownerOnlyOrAutonomousFalse_denied。
- applySelection_ticketConsumptionAndForegroundAcquisition_atomicRollback。
- applySelection_newUserInputOrStaleCatalog_rejectsOldChoice。
- applySelection_checkpointResume_preservesBudgetAndContext；userAndRestartPause_remainUserOnly。
- applySelection_smallEmotionChange_preservesCurrentActivity。
- applySelection_importantReply_preservesReadingCheckpoint。
- applySelection_emptyQueryWait_recordsCheckWithoutInventingConversation。
- selectActivity_noSuitableActivity_acceptsRest（happy path）；providerFailure_distinctFromWait。
- selectActivity_schemaRepairAndFallback_shareTwoActualSendLimit。
- selectionMaterials_unresolvedThought_hasRealSource；unavailableReportedAndPrivateMaterialExcludedExternally。
- aiBrain_consumerThroughRealRouterAndRuntime_startsActualRunner（生产接线，受控 provider）。
- aiBrain_configurationFailureReportedAndRecovery_doesNotPretendEnabled。
- memoryAccess_defaultClosedExplicitInternalScope_externalDoesNotInherit；relationsFromActualCues_doNotInventParticipants。

工程约定以主设计 v1.7 为准；先核对 Task 6 最终接口，不创建第二机会/预算权威。

Wave 7 校验（2026-10-17）：
- 文件冲突：通过，单 Task 实现 agent 串行拥有源码/tests/CMake/build，主 agent 维护设计与审查；阶段边界明确交回所有权。
- 构建/全量测试：完整应用和全部测试目标构建成功；最终 CTest **55/55 组通过**（75.44 秒），工具契约 **70/70**。选择器 21/21、任务执行器 30/30；没有删除或跳过失败断言。
- 行为覆盖：全部计划行为都有对应测试，见审查记录；包含真实两套包/操作名、真实记忆与新消息材料、默认/撤销权限、原子采用回滚、fallback/修复额度、安全检查点和真实空查询归档。
- 测试布局：通过，新增 tests/test_activity_selector.cpp 由根 CMakeLists.txt 注册，其他回归放既有对应套件。
- 实现锚点与留痕：通过，consumer、ModelRouter actual-attempt hooks、ActivityStore 原子采用、当前 grant validator、私有材料读取与 runner 检查点均核对；context.md 有记录。主设计 8 切片各 7 节、8 Task、38 本地链接和 SQL 迁移/重复/FK/空库撤销演练通过；git diff --check 干净。

最终日志：/tmp/task7-final-build.log、/tmp/task7-final-ctest.log、/tmp/task7-final-contract.log，结构化实现报告 /tmp/task7-implementation-report.md。Wave 7 五项通过；外部观察归一化与真实聊天等待链路由 Task 8 实施。

## Task 8: 外部观察与 QQ 式自主使用验证

- 关联设计: §3.9 外部观察与 QQ 式自主使用验证
- 状态: [ ]
- Wave: 8
- 依赖: Task 1—Task 7

文件：观察入口/连接模块、消息与活动协调器、受控消息插件 fixture、插件接入说明；新增 `tests/test_external_observation.cpp`、`tests/test_autonomous_chat.cpp`；`CMakeLists.txt`。

测试用例：
- ingestObservation_pollAndPushSameMessage_oneMessageAndOrigin（happy path）。
- setObservationSource_enableReconnectDisable_preservesCommittedCursor（happy path）。
- autonomousChat_checkDiscussWaitReadAndResume_preservesPeopleAndContexts（happy path）。
- autonomousChat_replaceChatPlugin_noCoreSoftwareSpecificChanges。
- autonomousChat_noBackgroundSource_proactiveQueryStillWorks。

真实 QQ 验收须单列具体后端、账号及实际结果；受控插件通过不等于真实 QQ 已接通。
