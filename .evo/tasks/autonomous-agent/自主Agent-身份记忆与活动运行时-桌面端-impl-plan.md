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
- 状态: [ ]
- Wave: 2
- 依赖: Task 1

文件：`core/ai/memory/` 中抽取、策略、存储、召回、活跃池及巩固；`core/ai/reflection/memory_consolidation_service.*`；`tests/test_memory_strategy.cpp`、`tests/test_memory_recall.cpp`、新增 `tests/test_memory_attribution.cpp`；`CMakeLists.txt`。

测试用例：
- ingestMemory_twoSpeakersSamePreference_separateAttribution（happy path）。
- ingestMemory_preferenceChanged_onlySamePersonConflicts。
- ingestMemory_reportedClaim_preservesSpeakerAndSubjectAfterConsolidation。
- recallForContext_allowedSharedKnowledge_recalls（happy path）。
- recallForContext_privateConversation_excludesOtherConversationsAndGraphPaths。
- migrateAttribution_repeatedBatch_preservesIdsSourcesAndProgress（happy path）。
- migrateAttribution_ambiguousLegacyText_remainsUnresolvedAndOwnerOnly。

## Task 3: 会话输入、上下文准备与回复定位

- 关联设计: §3.4 会话输入、上下文准备与回复定位
- 状态: [ ]
- Wave: 3
- 依赖: Task 1、Task 2

文件：`core/ai/chat/` 新增会话目录、`core/ai/chat/profile_chat_history_store.h/.cpp`、`core/ai/chat/chat_preparation_types.h`、`core/ai/chat/chat_preparation_executor.h/.cpp`、`core/ai/context/context_assembler.h/.cpp`、`core/ai/ai_brain.h` 及聊天实现；`tests/test_chat_preparation_executor.cpp`、`tests/test_chat_history.cpp`、新增 `tests/test_conversation_routing.cpp`；`CMakeLists.txt`。

测试用例：
- acceptMessage_remoteMessageRetry_persistsOnce（happy path）。
- acceptMessage_sameLocalMessageIdAcrossGroups_isolated。
- prepareContext_switchConversations_restoresOwnHistoryAndTopic（happy path）。
- publishResponse_newMessageInOtherChat_keepsOriginalTarget（happy path）。
- publishResponse_staleGeneration_rejectsSendButArchivesActualReceipt。

## Task 4: 插件发现、按需加载与通用执行

- 关联设计: §3.5 插件发现、按需加载与通用执行
- 状态: [ ]
- Wave: 4
- 依赖: Task 3

文件：`core/ai/tool_registry.h/.cpp`、`core/ai/tools/runtime/tool_runtime.h/.cpp`、`core/ai/tools/runtime/task_tool_executor.h/.cpp`、`core/ai/task/task_orchestrator.h/.cpp`、`core/ai/mcp/mcp_client.h/.cpp`、`core/ai/mcp/mcp_tool_adapter.h/.cpp`、`core/ai/skill/skill_store.h/.cpp`、新增插件目录与 Skill 加载模块、`contracts/tools/` 清单校验及示例；`tests/test_tool_registry.cpp`、新增 `tests/test_plugin_runtime.cpp`；`CMakeLists.txt`。

测试用例：
- registerPlugin_skillToolAndMixedPackages_indexesSummaries（happy path）。
- loadCapability_skillThenTool_loadsOnlyRequestedDetails（happy path）。
- invokeCapability_scriptCliAndControlledMcp_returnsActualOutcome（happy path）。
- invokeCapability_emptyAcceptedFailedCompleted_distinguishesOutcomes。
- setPluginEnabled_disableThenEnable_blocksNewCallsAndInvalidatesOldCache（happy path）。
- setPluginEnabled_inFlightReceipt_keepsOriginalCallIdentity。

## Task 5: 活动执行、挂起、恢复与经历

- 关联设计: §3.6 活动执行、挂起、恢复与经历
- 状态: [ ]
- Wave: 5
- 依赖: Task 3、Task 4

文件：新增 `core/ai/activity/`、`core/ai/task/`、`core/ai/runtime/`、runtime schema；`tests/test_task_orchestrator.cpp`、`tests/test_task_experience.cpp`、新增 `tests/test_activity_runtime.cpp`；`CMakeLists.txt`。

测试用例：
- startActivity_multistepRead_holdsForegroundAcrossSteps（happy path）。
- transitionActivity_sentReceiptWaiting_releasesForeground（happy path）。
- resumeActivity_matchingReply_resumesChatThenReadingCheckpoint（happy path）。
- observeAction_verifiedLateReceipt_settlesOnceWithoutResend（happy path）。
- resumeActivity_restart_keepsBudgetsAndRequiresValidResumeAuthority。
- transitionActivity_emptyCheckCompleted_modelFailureNotCompleted。

## Task 6: 空闲判定与自主机会调度

- 关联设计: §3.7 空闲判定与自主机会调度
- 状态: [ ]
- Wave: 6
- 依赖: Task 5

文件：新增通用机会调度模块、`core/ai/ai_brain_loop.cpp`、`core/ai/scheduler/`、`core/ai/runtime/`；`tests/test_agent_scheduler.cpp`、新增 `tests/test_activity_opportunities.cpp`；`CMakeLists.txt`。

测试用例：
- evaluateIdle_onlyExternalWaitOrBackgroundMemory_returnsIdle（happy path）。
- evaluateIdle_toolWaitValidationOrReadyObligation_returnsBusy。
- considerOpportunity_idleEnabledBudgeted_reservesOneTicket（happy path）。
- considerOpportunity_paused_noModelCall。
- considerOpportunity_messageBurst_coalescesOnlySameConversationWithinBound。
- considerOpportunity_unchangedIdle_backsOffUntilImportantEvent。

## Task 7: 自主活动选择与持续意图

- 关联设计: §3.8 自主活动选择与持续意图
- 状态: [ ]
- Wave: 7
- 依赖: Task 1—Task 6

文件：新增活动选择模块、`core/ai/task/agency_decision_service.*` 集成、身份/内心材料投影、能力目录；新增 `tests/test_activity_selector.cpp`；`CMakeLists.txt`。

测试用例：
- selectActivity_relevantFriendMemory_proposesEvidenceBasedCheck（happy path）。
- applySelection_newReadingCapability_startsWithoutSoftwareNameBranch（happy path）。
- applySelection_smallEmotionChange_preservesCurrentActivity。
- applySelection_importantReply_preservesReadingCheckpoint。
- applySelection_emptyQueryWait_recordsCheckWithoutInventingConversation。
- selectActivity_noSuitableActivity_acceptsRest。

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
