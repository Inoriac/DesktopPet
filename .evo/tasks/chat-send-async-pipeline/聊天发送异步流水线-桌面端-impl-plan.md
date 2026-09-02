# 聊天发送异步流水线实现计划

**系分文档**: `聊天发送异步流水线-桌面端系分.md`

---

## Wave 1

### Task 1: 异步聊天准备与模型派发

- **关联设计**: §3.2 异步聊天准备与模型派发
- **状态**: [ ]
- **Wave**: 1
- **依赖**: 无

**文件**:

- 实现: `core/ai/chat/chat_preparation_types.h`
- 实现: `core/ai/chat/chat_preparation_executor.h`
- 实现: `core/ai/chat/chat_preparation_executor.cpp`
- 实现: `core/ai/ai_brain.h`
- 实现: `core/ai/ai_brain.cpp`
- 实现: `core/ai/ai_brain_router.cpp`
- 实现: `core/ai/memory/memory_retriever.h`
- 实现: `core/ai/memory/memory_retriever.cpp`
- 构建: `CMakeLists.txt`
- 测试: `tests/test_chat_preparation_executor.cpp`
- 测试: `tests/test_streaming_dialogue.cpp`

**测试用例**:

`ChatPreparationExecutor::start`:

- `start_whenEnvironmentIsValid_shouldCreateWorkerOwnedResources`（happy path）
- `start_whenDatabasePathsAreInvalid_shouldReturnControlledFailure`

`ChatPreparationExecutor::submit`:

- `submit_whenContextIsValid_shouldReturnMessagesAndReinforcementIds`（happy path）
- `submit_whenIdentityDatabaseIsUnavailable_shouldUseBaselinePersona`
- `submit_whenMemoryDatabaseIsUnavailable_shouldReturnContextWithoutPersistentHints`

`AIBrain::triggerThink` / `continuePreparedThink`:

- `triggerThink_whenPreparationIsDelayed_shouldKeepGuiEventLoopResponsiveAndDispatchOnce`（happy path）
- `triggerThink_whenStoppedBeforePreparationCompletes_shouldDiscardStaleResult`
- `triggerThink_whenPreparationFails_shouldFinishResponseAndClearBusyState`
- `triggerThink_whenLocalRouterHandlesRequest_shouldPreserveFastPath`

`MemoryRetriever::retrieve`:

- `retrieve_whenMemoriesMatch_shouldReturnRankedResultsWithoutPersistenceMutation`（happy path）

---

## Wave 2

### Task 2: 延迟副作用与性能保障

- **关联设计**: §3.3 延迟副作用与性能保障
- **状态**: [ ]
- **Wave**: 2
- **依赖**: Task 1

**文件**:

- 实现: `core/ai/chat/chat_side_effect_queue.h`
- 实现: `core/ai/chat/chat_side_effect_queue.cpp`
- 实现: `core/ai/ai_brain.h`
- 实现: `core/ai/ai_brain.cpp`
- 实现: `core/ai/ai_brain_loop.cpp`
- 实现: `core/ai/memory/memory_store.h`
- 实现: `core/ai/memory/memory_store.cpp`
- 实现: `core/ai/ai_call_logger.h`
- 实现: `core/ai/ai_call_logger.cpp`
- 实现: `core/ai/runtime/agent_runtime_services.h`
- 实现: `core/ai/runtime/agent_runtime_services.cpp`
- 构建: `CMakeLists.txt`
- 文档: `README.md`
- 测试: `tests/test_chat_side_effect_queue.cpp`
- 测试: `tests/test_memory_strategy.cpp`
- 测试: `tests/test_streaming_dialogue.cpp`
- 测试: `tests/test_agent_runtime_services.cpp`

**测试用例**:

`MemoryStore::stageReinforcement`:

- `stageReinforcement_whenIdsExist_shouldUpdateMemoryAndReturnOneBatch`（happy path）
- `stageReinforcement_whenIdsRepeatOrAreMissing_shouldUpdateEachKnownEntryOnce`

`ChatSideEffectQueue::start`:

- `start_whenEnvironmentIsValid_shouldOpenThreadOwnedPersistenceResources`（happy path）
- `start_whenOptionalLogPathIsUnavailable_shouldKeepDatabaseEffectsAvailable`

`ChatSideEffectQueue::enqueue` / `enqueueBarrier`:

- `enqueue_whenSessionHasOrderedEffects_shouldPersistBeforeBarrier`（happy path）
- `enqueue_whenReinforcementContainsEightEntries_shouldUseOneTransaction`
- `enqueue_whenLogWriteFails_shouldWarnWithoutBlockingLaterEffects`
- `enqueueBarrier_whenGenerationIsStale_shouldNotMutateActiveResponse`

`AIBrain::thinkInternal` / `finishActiveResponse`:

- `thinkInternal_whenRequestIsPrepared_shouldDispatchBeforeNonCriticalEffectsComplete`（happy path）
- `finishActiveResponse_whenEventsAreQueued_shouldReflectOnlyAfterBarrier`
- `stop_whenSideEffectsArePending_shouldNotDeliverCallbacksToDestroyedState`

`Chat UI responsiveness`:

- `messageSend_whenPreparationTakesOneHundredMilliseconds_shouldAllowSixteenMillisecondTimerToAdvance`（happy path）
