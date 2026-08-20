# 桌宠自主迭代桌面端实现计划

**系分文档**: `.evo/tasks/desktop-pet-self-evolution/桌宠自主迭代-桌面端系分.md`

---

## Task 1: 事件账本与运行时服务组装

- **关联设计**: §3.2 事件账本与运行时服务组装
- **状态**: [ ]
- **Wave**: 1
- **依赖**: 无

**文件**:
- 实现: `core/ai/domain/domain_result.h`
- 实现: `core/ai/event/event_types.*`、`core/ai/event/event_ledger.*`、`core/ai/event/sqlite_event_repository.*`
- 实现: `core/ai/runtime/agent_runtime_services.*`、`core/ai/runtime/agent_bootstrap.*`
- 修改: `core/ai/agent/agent_session.*`、`core/ai/ai_brain.*`、`core/ai/ai_brain_loop.cpp`、`core/ai/ai_brain_router.cpp`
- 修改: `ui/petwindow_ai.cpp`、`ui/petwindow.h`、`CMakeLists.txt`
- 测试: `tests/test_event_ledger.cpp`

**测试用例**（按被测方法分组，每方法至少 1 条 happy path）:

EventLedger.append:
- `append_whenDraftIsValid_shouldPersistAndReturnSequencedEvent`（happy path）
- `append_whenOccurredAtMissing_shouldUseUtcClock`
- `append_whenPrivatePayloadContainsBody_shouldPersistReferenceOnly`
- `append_whenSchemaInvalid_shouldRejectWithoutPartialRow`

EventLedger.readAfter:
- `readAfter_whenEventsMatchFilter_shouldReturnOrderedBoundedBatch`（happy path）
- `readAfter_whenConsumerCannotReadPrivate_shouldReturnOnlyAllowedReference`

EventConsumerCheckpointStore.commit:
- `commit_whenExpectedSequenceMatches_shouldAdvanceCheckpoint`（happy path）
- `commit_whenExpectedSequenceConflicts_shouldPreserveCurrentCheckpoint`

AgentRuntimeServices.start:
- `start_whenStoresAreAvailable_shouldMigrateAndAssembleServices`（happy path）
- `start_whenNewSchemaMigrationFails_shouldKeepLegacyChatToolAndSkillPathAvailable`

AgentRuntimeServices.captureSnapshot:
- `captureSnapshot_whenSessionStarts_shouldPinIdentityAndConfigVersions`（happy path）
- `captureSnapshot_whenStateChangesLater_shouldKeepExistingSessionProjectionStable`

AgentRuntimeServices.stop:
- `stop_whenRuntimeIsActive_shouldRejectNewSessionsAndCloseServices`（happy path）
- `stop_whenCalledTwice_shouldRemainIdempotent`

## Task 2: 多模型路由与上下文投影

- **关联设计**: §3.3 多模型路由与上下文投影
- **状态**: [ ]
- **Wave**: 2
- **依赖**: Task 1

**文件**:
- 修改: `include/ai_types.h`、`core/configLoader/config_manager.*`
- 实现: `core/ai/model/model_router.*`、`core/ai/model/model_role_registry.*`
- 实现/修改: `core/ai/context/context_assembler.*`、`core/ai/context/context_manager.*`、`core/ai/context_builder.*`
- 修改: `core/ai/llm/llm_chat_service.*`、`statistic/statistic_manager.*`、`include/statistic_types.h`
- 修改: `config/default_common_config.json`、`config/default_common_config.example.json`
- 修改: `launcher/app_state.py`、`launcher/config_loader.py`、`launcher/pages/ai_page.py`
- 修改: `CMakeLists.txt`
- 测试: `tests/test_model_router.cpp`
- 测试: `launcher/tests/test_model_role_config.py`（launcher 首次引入 Python 测试，采用该模块下 `tests/test_*.py` 的标准 unittest 布局）

**测试用例**（按被测方法分组，每方法至少 1 条 happy path）:

ModelRouter.resolve:
- `resolve_whenPrimaryRouteMeetsConstraints_shouldReturnPrimary`（happy path）
- `resolve_whenPrimaryIsOpenCircuit_shouldReturnConfiguredFallback`
- `resolve_whenNoRouteSupportsVision_shouldReturnRoleUnavailable`

ModelRouter.completeAsync:
- `completeAsync_whenPrimarySucceeds_shouldReturnValidatedResponseAndRoleDimensions`（happy path）
- `completeAsync_whenOutputSchemaInvalidOnce_shouldRepairThenReturnValidResponse`
- `completeAsync_whenProviderTimesOut_shouldTryFallbackWithoutChangingContextScope`

ContextAssembler.assemble:
- `assemble_whenDialogueRole_shouldIncludePersonaMemoryAndSkillSummary`（happy path）
- `assemble_whenDialogueRole_shouldExcludeDiaryInnerThoughtAndOwnerAccess`
- `assemble_whenDiaryRole_shouldExposeOnlyDiaryProjectionWithinBudget`
- `assemble_whenRequestedPartitionIsForbidden_shouldReturnScopeDenied`

ConfigManager.getModelRoleConfig:
- `getModelRoleConfig_whenRoleConfigured_shouldReturnRoleSpecificModel`（happy path）
- `getModelRoleConfig_whenOnlyLegacyConfigExists_shouldMapItToDialogue`

Launcher model-role config export:
- `export_model_roles_whenMultipleProfilesConfigured_shouldPreserveEachRoleAndFallback`（happy path）
- `export_model_roles_whenApiKeyPresent_shouldKeepExistingSecretHandlingBehavior`

## Task 3: 情绪接入、人格、关系与自我模型

- **关联设计**: §3.4 情绪接入、人格、关系与自我模型
- **状态**: [ ]
- **Wave**: 3
- **依赖**: Task 1、Task 2

**文件**:
- 实现: `core/ai/integration/emotion_state_provider.*`
- 实现: `core/ai/identity/identity_baseline.*`、`core/ai/identity/identity_types.*`
- 实现: `core/ai/identity/personality_service.*`、`relationship_service.*`、`self_model_service.*`、`persona_projector.*`、`sqlite_identity_repository.*`
- 修改: `core/ai/context/context_manager.*`、`core/configLoader/config_manager.*`
- 修改: `config/default_common_config.json`、`config/default_common_config.example.json`
- 修改: `CMakeLists.txt`
- 测试: `tests/test_emotion_provider_contract.cpp`
- 测试: `tests/test_identity_state.cpp`

**测试用例**（按被测方法分组，每方法至少 1 条 happy path）:

NullEmotionStateProvider.currentSnapshot:
- `currentSnapshot_whenCalled_shouldReturnVersionedNeutralSnapshotWithoutSideEffects`（happy path）

NullEmotionStateProvider.trajectory:
- `trajectory_whenRangeIsValid_shouldReturnEmptyWithoutStorageOrModelCalls`（happy path）

PersonalityService.recordEvidence:
- `recordEvidence_whenExplicitCorrectionArrives_shouldPersistTraceableWeightedEvidence`（happy path）
- `recordEvidence_whenSameSourceIsRepeated_shouldDeduplicateEvidence`

PersonalityService.consolidate:
- `consolidate_whenIndependentLongWindowEvidencePassesThreshold_shouldAppendLimitedStateVersion`（happy path）
- `consolidate_whenOnlyNeutralEmotionExists_shouldNotMoveBaseline`
- `consolidate_whenVersionConflictsTwice_shouldLeaveEvidencePending`

PersonalityService.rollback:
- `rollback_whenTargetVersionExists_shouldAppendRestoredVersionWithoutDeletingHistory`（happy path）

RelationshipService.applyEvidence:
- `applyEvidence_whenOwnerEvidenceArrives_shouldChangeOnlyOwnerRelationship`（happy path）

SelfModelService.evolve:
- `evolve_whenProposalHasCommittedIndependentEvidence_shouldAppendNarrativeVersion`（happy path）
- `evolve_whenNarrativeReferencesOnlyItself_shouldRejectCircularEvidence`

PersonaProjector.project:
- `project_whenSessionSnapshotIsValid_shouldMergeBaselineRelationshipAndBoundedSlots`（happy path）
- `project_whenNullEmotionProviderUsed_shouldOmitEmotionPromptAndPrivateInternals`
- `project_whenReminderPersonalityExists_shouldNotReadOrMutateReminderSettings`

## Task 4: Daydream、内心活动、日记与睡眠循环

- **关联设计**: §3.5 Daydream、内心活动、日记与睡眠循环
- **状态**: [ ]
- **Wave**: 4
- **依赖**: Task 1、Task 2、Task 3

**文件**:
- 实现: `core/ai/reflection/reflection_types.*`、`cancellation_token.*`、`private_key_provider.*`、`private_psyche_crypto.*`、`sqlite_private_psyche_repository.*`
- 实现: `core/ai/reflection/inner_thought_service.*`、`daydream_service.*`、`diary_service.*`、`sleep_cycle_coordinator.*`
- 修改: `core/ai/memory/*`、`core/ai/scheduler/agent_scheduler.*`、`core/ai/ai_brain.h`
- 修改: `CMakeLists.txt`（libsodium、Qt6Keychain、reflection 测试目标）
- 测试: `tests/test_sleep_cycle.cpp`

**测试用例**（按被测方法分组，每方法至少 1 条 happy path）:

InnerThoughtService.createAsync:
- `createAsync_whenHighValueEventCompletes_shouldStageShortPrivateSummaryWithoutBlockingReply`（happy path）
- `createAsync_whenCallbackArrivesAfterCancellation_shouldDiscardResult`
- `createAsync_whenModelReturnsReasoningTrace_shouldPersistOnlyRequestedSummaryFields`

DaydreamService.consolidateAsync:
- `consolidateAsync_whenPendingItemsExist_shouldStageBoundedDeduplicatedChanges`（happy path）
- `consolidateAsync_beforeCommit_shouldLeaveFormalMemoryUnchanged`

DiaryService.composeAsync:
- `composeAsync_whenBedtimeContextIsValid_shouldStageOneEncryptedDiaryForLocalDate`（happy path）
- `composeAsync_whenDateAlreadyCommitted_shouldReturnExistingEntryId`
- `composeAsync_whenModelOutputIsEmptyAfterRepair_shouldKeepRetryablePendingState`
- `composeAsync_whenKeychainUnavailable_shouldReturnPrivateStoreUnavailableWithoutPlaintextFallback`

DiaryService.readForSelf:
- `readForSelf_whenDiaryRoleHasEntryScope_shouldDecryptSelectedEntry`（happy path）
- `readForSelf_whenDialogueScopeRequestsEntry_shouldReturnScopeDenied`

DiaryService.readForOwner:
- `readForOwner_whenAuthAndProfileMatch_shouldDecryptSingleEntry`（happy path）
- `readForOwner_whenAadOrCiphertextIsModified_shouldRejectAuthentication`

SleepCycleCoordinator.tryStart:
- `tryStart_whenBedtimeIdleAndNoDueTask_shouldCreateDurablePendingSession`（happy path）
- `tryStart_whenBrainBusyOrTaskDueSoon_shouldNotStart`
- `tryStart_whenAllParticipantsPrepared_shouldPersistCommitThenFinalizeAllStores`
- `tryStart_whenRestartFindsCommittedSession_shouldIdempotentlyFinishFinalize`

SleepCycleCoordinator.cancel:
- `cancel_whenDecisionPending_shouldAbortAllStagingAndPreserveFormalState`（happy path）
- `cancel_whenDecisionCommitted_shouldKeepCommitAndFinishFinalize`

## Task 5: OwnerDiaryServer 与 launcher 私有日记页

- **关联设计**: §3.6 OwnerDiaryServer 与 launcher 私有日记页
- **状态**: [ ]
- **Wave**: 5
- **依赖**: Task 4

**文件**:
- 实现: `core/ai/owner/owner_diary_protocol.*`、`owner_diary_facade.*`、`owner_diary_server.*`
- 修改: `core/ai/runtime/agent_bootstrap.*`、`main.cpp`
- 实现: `launcher/owner_diary_client.py`、`launcher/pages/private_diary_page.py`
- 修改: `launcher/main.py`
- 修改: `CMakeLists.txt`
- 测试: `tests/test_owner_diary_server.cpp`
- 测试: `launcher/tests/test_owner_diary_client.py`

**测试用例**（按被测方法分组，每方法至少 1 条 happy path）:

OwnerDiaryFacade.list:
- `list_whenOwnerSessionValid_shouldReturnPagedMetadataWithoutBody`（happy path）
- `list_whenProfileDoesNotMatch_shouldReturnGenericNotFound`

OwnerDiaryFacade.get:
- `get_whenOwnerSessionValid_shouldReturnOneDecryptedEntry`（happy path）
- `get_whenEntryBelongsToAnotherProfile_shouldNotRevealExistence`

OwnerDiaryServer.listen:
- `listen_whenBootstrapIsValid_shouldConsumeSecretAndAcceptAuthenticatedHello`（happy path）
- `listen_whenBootstrapOwnerOrExpiryInvalid_shouldRejectAndNotBindSocket`
- `listen_whenTokenIsForgedOrReplayed_shouldRejectSession`

OwnerDiaryServer protocol handling:
- `handleFrame_whenListOrGetActionValid_shouldReturnMatchingReadOnlyResponse`（happy path）
- `handleFrame_whenActionIsWriteSqlPathSkillOrTool_shouldReturnActionNotAllowed`
- `handleFrame_whenFrameTooLargeOrVersionInvalid_shouldRejectWithoutAllocationGrowth`
- `handleFrame_whenRequestIdIsRetried_shouldReturnIdempotentReadResponse`

OwnerDiaryServer.stop:
- `stop_whenSessionsExist_shouldCloseSocketAndRevokeSessionTokens`（happy path）

OwnerDiaryClient.connect/list_entries/get_entry/close:
- `connect_then_list_and_get_whenServerAvailable_shouldRenderMetadataThenSingleBody`（happy path）
- `close_whenBodyLoaded_shouldClearTokenAndDecryptedContent`
- `request_whenServerOffline_shouldShowOfflineWithoutOpeningSQLite`

Model invisibility regression:
- `toolRegistry_whenOwnerDiaryEnabled_shouldNotExposeOwnerDiaryActions`（happy path）
- `dialogueContext_whenOwnerReadsDiary_shouldNotContainAccessEventOrDiaryBody`
