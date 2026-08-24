# 桌宠自主迭代桌面端实现计划

**系分文档**: `.evo/tasks/desktop-pet-self-evolution/桌宠自主迭代-桌面端系分.md`

---

## Task 1A: 基线对齐、角色身份与数据迁移

- **关联设计**: §3.2 事件账本与运行时服务组装（基线与迁移子接口）；共享约束见 §1.8、§5.1、§6.5、§7.3
- **状态**: [x]
- **Wave**: 1
- **依赖**: 无

**文件**:
- 实现: `core/ai/runtime/profile_resolver.*`、`core/ai/runtime/profile_data_migrator.*`
- 修改: `launcher/pet_registry.py`、`launcher/app_state.py`、`launcher/main.py`、`launcher/pages/pet_page.py`
- 修改: `entity/pet.*`、`main.cpp`、`core/ai/memory/memory_store.*`
- 修改: `CMakeLists.txt`（libsodium、QtKeychain 可选探测与能力宏，不要求本 Task 实现私有存储）
- 测试: `tests/test_profile_data_migrator.cpp`
- 测试: `tests/test_pet_profile_id.py`（沿用仓库现有 `tests/test_*.py` 布局）

**测试用例**（按被测方法分组，每方法至少 1 条 happy path）:

Pet registry migration:
- `load_pets_whenLegacyEntriesExist_shouldAssignAndPersistStableProfileIds`（happy path）
- `rename_pet_whenProfileExists_shouldPreserveProfileId`
- `load_pets_whenExistingProfileIdsAreDuplicated_shouldRejectWithoutRewrite`

ProfileResolver.resolve:
- `resolve_whenPetAndProfileIdMatch_shouldReturnProfileWithoutOpeningStores`（happy path）
- `resolve_whenPetAndProfileIdDoNotMatch_shouldRejectBeforeOpeningStores`
- `resolve_whenProfileIdArgumentIsMissing_shouldResolveItFromPetRegistry`
- `resolve_whenLegacyRegistryHasNoProfileId_shouldRequireLauncherUpgrade`
- `resolve_whenRegistryProfileFieldsAreInvalid_shouldRejectEntireMapping`
- `load_whenAnyRegistryEntryIsMalformed_shouldRejectEntireRegistry`

ProfileDataMigrator.migrateLegacyMemory:
- `migrateLegacyMemory_whenSingleProfileAndValidLegacyDb_shouldCopyVerifyAndAtomicallyActivate`（happy path）
- `migrateLegacyMemory_whenMultipleProfilesAreAmbiguous_shouldPreserveLegacyDbAndRequireSelection`
- `migrateLegacyMemory_whenIntegrityCheckFails_shouldRemoveTempCopyAndKeepLegacyPath`
- `migrateLegacyMemory_whenTargetAlreadyCommitted_shouldRemainIdempotent`
- `migrateLegacyMemory_whenOnlyLegacyJsonExists_shouldImportIntoProfileDatabase`
- `migrateLegacyMemory_whenRegisteredProfilesAreInvalid_shouldRejectRequest`

**静态验收**:
- CMake 缺少 libsodium 或 QtKeychain 时仍生成非私有目标，并把 private reflection capability 设为 unavailable；运行时降级行为由 Task 4 测试。

## Task 1B: 事件账本与运行时服务组装

- **关联设计**: §3.2 事件账本与运行时服务组装
- **状态**: [x]
- **Wave**: 2
- **依赖**: Task 1A

**文件**:
- 实现: `core/ai/domain/domain_result.h`
- 实现: `core/ai/event/event_types.*`、`core/ai/event/event_schema_registry.*`、`core/ai/event/event_ledger.*`、`core/ai/event/event_outbox.*`、`core/ai/event/runtime_unit_of_work.*`、`core/ai/event/sqlite_event_repository.*`
- 实现: `core/ai/runtime/runtime_types.h`、`core/ai/runtime/runtime_ui_bridge.*`、`core/ai/runtime/agent_runtime_services.*`、`core/ai/runtime/agent_bootstrap.*`
- 修改: `core/ai/agent/agent_session.*`、`core/ai/ai_brain.*`、`core/ai/ai_brain_loop.cpp`、`core/ai/ai_brain_router.cpp`
- 修改: `core/configLoader/config_manager.*`
- 修改: `main.cpp`、`ui/petwindow.cpp`、`ui/petwindow_ai.cpp`、`ui/petwindow.h`、`CMakeLists.txt`
- 测试: `tests/test_event_ledger.cpp`
- 测试: `tests/test_agent_runtime_services.cpp`

**测试用例**（按被测方法分组，每方法至少 1 条 happy path）:

EventSchemaRegistry.registerSchema/validate:
- `validate_whenBuiltInSchemaMatches_shouldAcceptDraftAndPreserveUnknownFields`（happy path）
- `registerSchema_whenRegistryIsFrozen_shouldRejectWithoutReplacingDefinition`
- `validate_whenPrivateDraftContainsBodyOrInvalidReference_shouldReject`

EventLedger.append:
- `append_whenDraftIsValid_shouldPersistAndReturnSequencedEvent`（happy path）
- `append_whenOccurredAtMissing_shouldUseUtcClock`
- `append_whenPrivateReferenceIsValid_shouldPersistReferenceWithoutBody`
- `append_whenPrivatePayloadContainsBody_shouldRejectWithoutPartialRow`
- `append_whenSchemaInvalid_shouldRejectWithoutPartialRow`

EventLedger.readAfter:
- `readAfter_whenEventsMatchFilter_shouldReturnOrderedBoundedBatch`（happy path）
- `readAfter_whenConsumerCanReadPrivateReference_shouldReturnReferenceWithoutBody`
- `readAfter_whenConsumerCannotReadPrivate_shouldFilterPrivateEvent`
- `readAfter_whenAuthorizationProfileDoesNotMatch_shouldRejectRead`

EventConsumerCheckpointStore.current/commit:
- `commit_whenExpectedSequenceMatches_shouldAdvanceCheckpoint`（happy path）
- `commit_whenExpectedSequenceConflicts_shouldPreserveCurrentCheckpoint`

RuntimeUnitOfWork/EventOutbox.enqueue:
- `enqueue_whenDomainWriteUsesSameUnitOfWork_shouldCommitStateAndPendingEventTogether`（happy path）
- `enqueue_whenOutboxWriteFails_shouldRollBackDomainState`

EventOutbox.dispatchPending:
- `dispatchPending_whenDomainTransactionCommitted_shouldAppendEventAndMarkDelivered`（happy path）
- `dispatchPending_whenTransactionFailsAfterInsert_shouldRollBackEventAndKeepOutboxPending`
- `dispatchPending_whenAppendTemporarilyFails_shouldKeepPendingAndAdvanceRetryMetadata`

AgentBootstrap.start/AIBrain.initializeStorage:
- `start_whenMigrationIsReady_shouldConfigureActivePathsBeforeFirstMemoryStoreOpen`（happy path）
- `start_whenMigrationIsAmbiguous_shouldUseLegacyPathsAndDisableProfileGrowth`
- `start_whenNewSchemaMigrationFails_shouldKeepLegacyChatToolAndSkillPathAvailable`
- `initializeStorage_whenCalledTwice_shouldKeepFirstPathsAndRejectSecondCall`

AgentRuntimeServices.captureSnapshot:
- `captureSnapshot_whenSessionStarts_shouldPinIdentityAndConfigVersions`（happy path）
- `captureSnapshot_whenStateChangesLater_shouldKeepExistingSessionProjectionStable`

AgentSession.bindRuntimeSnapshot:
- `bindRuntimeSnapshot_whenSessionIdsMatch_shouldPinSnapshotOnce`（happy path）
- `bindRuntimeSnapshot_whenCalledTwice_shouldPreserveOriginalSnapshot`

ConfigManager.configHash/identityBaselineHash:
- `configHash_whenSameEffectiveJsonHasDifferentKeyOrder_shouldRemainDeterministic`（happy path）
- `identityBaselineHash_whenBaselineChanges_shouldProduceDifferentVersionHash`

AgentRuntimeServices.stop:
- `stop_whenRuntimeIsActive_shouldRejectNewSessionsAndCloseServices`（happy path）
- `stop_whenCalledTwice_shouldRemainIdempotent`

RuntimeUiBridge lifetime contract:
- `start_whenBridgeIsValid_shouldUseUiCapabilitiesWithoutTakingOwnership`
- `stop_whenRuntimeEnds_shouldReleaseBridgeBeforePetWindowDestroysUiObjects`

## Task 2: 多模型路由与上下文投影

- **关联设计**: §3.3 多模型路由与上下文投影
- **状态**: [ ]
- **Wave**: 3
- **依赖**: Task 1B

**文件**:
- 修改: `include/ai_types.h`、`core/configLoader/config_manager.*`
- 实现: `core/ai/model/model_router.*`、`core/ai/model/model_role_registry.*`
- 实现: `core/ai/context/context_assembler.*`
- 实现: `core/ai/llm/llm_chat_model_client.*`
- 修改: `core/ai/ai_brain.h`、`core/ai/ai_brain_loop.cpp`（仅接入 Dialogue role，保留现有 tool round/session/事件行为）
- 修改: `config/default_common_config.json`、`config/default_common_config.example.json`
- 修改: `launcher/config_loader.py`（现有基础 UI 同步 Dialogue/Vision 首 route，其他 route 原样保留）
- 修改: `CMakeLists.txt`
- 测试: `tests/test_model_router.cpp`
- 测试: `tests/test_model_role_config.py`（沿用仓库现有 `tests/test_*.py` 布局）

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

**MVP 边界**:
- 仅实现有序 route fallback、单次 repair、进程内短暂熔断、顶层 JSON Object 基本类型校验和上下文分区白名单。
- 本 Task 仅把现有对话主链接入 Dialogue role；Consolidation/Diary 的领域数据源由后续 Task 通过 `ContextProjection` 传入。

## Task 3: 情绪接入、人格、关系与自我模型

- **关联设计**: §3.4 情绪接入、人格、关系与自我模型
- **状态**: [ ]
- **Wave**: 4
- **依赖**: Task 1B、Task 2

**文件**:
- 实现: `core/ai/integration/emotion_state_provider.*`
- 修改: `core/ai/identity/identity_baseline.*`、`persona_projector.*`（扩展现有类型和兼容入口）
- 实现: `core/ai/identity/identity_types.*`、`personality_service.*`、`relationship_service.*`、`self_model_service.*`、`sqlite_identity_repository.*`
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

EmotionEngineStateProvider.currentSnapshot:
- `currentSnapshot_whenEngineHasValidState_shouldReturnReadOnlyMappedSnapshot`（happy path）
- `currentSnapshot_whenEngineIsUnavailable_shouldFallBackWithoutMutatingEmotionState`

EmotionEngineStateProvider.trajectory:
- `trajectory_whenCurrentEngineHasNoHistoryContract_shouldReturnEmptyWithoutSchemaChanges`（happy path）

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

## Task 4: 现有 Daydream 编排、内心活动、日记与睡眠循环

- **关联设计**: §3.5 Daydream、内心活动、日记与睡眠循环
- **状态**: [ ]
- **Wave**: 5
- **依赖**: Task 1B、Task 2、Task 3

**文件**:
- 实现: `core/ai/reflection/reflection_types.*`、`cancellation_token.*`、`private_key_provider.*`、`private_psyche_crypto.*`、`sqlite_private_psyche_repository.*`
- 实现: `core/ai/reflection/inner_thought_service.*`、`daydream_sleep_adapter.*`、`diary_service.*`、`sleep_cycle_coordinator.*`
- 修改: `core/ai/memory/*`、`core/ai/scheduler/agent_scheduler.*`、`core/ai/ai_brain.h`
- 修改: `CMakeLists.txt`（libsodium、Qt6Keychain、reflection 测试目标）
- 测试: `tests/test_sleep_cycle.cpp`

**测试用例**（按被测方法分组，每方法至少 1 条 happy path）:

InnerThoughtService.createAsync:
- `createAsync_whenHighValueEventCompletes_shouldStageShortPrivateSummaryWithoutBlockingReply`（happy path）
- `createAsync_whenCallbackArrivesAfterCancellation_shouldDiscardResult`
- `createAsync_whenModelReturnsReasoningTrace_shouldPersistOnlyRequestedSummaryFields`

DaydreamSleepAdapter.consolidateAsync:
- `consolidateAsync_whenPendingItemsExist_shouldReuseExistingConsolidatorAndStageBoundedChanges`（happy path）
- `consolidateAsync_beforeCommit_shouldLeaveFormalMemoryUnchanged`
- `consolidateAsync_whenCancelled_shouldFollowExistingGenerationAndRollbackRules`

DaydreamConsolidator.buildChangeSet/applyChangeSet:
- `buildChangeSet_whenDecisionsAreValid_shouldReturnDeterministicChangesWithoutMutatingStore`（happy path）
- `applyChangeSet_whenCommitIsDurable_shouldMaterializeChangesOnce`
- `applyDecisions_whenCalledByLegacyDaydream_shouldDelegateAndPreserveExistingBehavior`

DiaryService.composeAsync:
- `composeAsync_whenBedtimeContextIsValid_shouldStageOneEncryptedDiaryForLocalDate`（happy path）
- `composeAsync_whenDateAlreadyCommitted_shouldReturnExistingEntryId`
- `composeAsync_whenModelOutputIsEmptyAfterRepair_shouldKeepRetryablePendingState`
- `composeAsync_whenKeychainUnavailable_shouldReturnPrivateStoreUnavailableWithoutPlaintextFallback`
- `privateServices_whenBuildCapabilityIsUnavailable_shouldReturnPrivateStoreUnavailableWithoutPlaintextFallback`

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
- **Wave**: 6
- **依赖**: Task 4

**文件**:
- 实现: `core/ai/owner/owner_diary_protocol.*`、`owner_diary_facade.*`、`owner_diary_server.*`
- 修改: `core/ai/runtime/agent_bootstrap.*`、`main.cpp`
- 实现: `launcher/owner_diary_client.py`、`launcher/pages/private_diary_page.py`
- 修改: `launcher/main.py`
- 修改: `CMakeLists.txt`
- 测试: `tests/test_owner_diary_server.cpp`
- 测试: `tests/test_owner_diary_client.py`（沿用仓库现有 `tests/test_*.py` 布局）

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
- `pageDeactivate_whenBodyLoaded_shouldClearContentAndKeepLauncherSession`
- `close_whenLauncherExits_shouldClearTokenAndDecryptedContent`
- `request_whenServerOffline_shouldShowOfflineWithoutOpeningSQLite`
- `connect_whenLauncherWasRestartedWithOldCoreRunning_shouldNotReuseConsumedTokenOrGuessSocket`

Model invisibility regression:
- `toolRegistry_whenOwnerDiaryEnabled_shouldNotExposeOwnerDiaryActions`（happy path）
- `dialogueContext_whenOwnerReadsDiary_shouldNotContainAccessEventOrDiaryBody`
