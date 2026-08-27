# 桌宠流式聊天与 Fluent 界面重构实现计划

**系分文档**: `桌宠流式聊天与Fluent界面重构-桌面端系分.md`

---

## Task 1: Anthropic Messages 流式客户端

- **关联设计**: §3.2 Anthropic Messages 流式客户端
- **状态**: [x]
- **Wave**: 1
- **依赖**: 无

**文件**:
- 实现: `core/ai/llm/llm_stream_types.h`
- 实现: `core/ai/llm/sse_event_parser.h`
- 实现: `core/ai/llm/sse_event_parser.cpp`
- 实现: `core/ai/llm/anthropic_messages_client.h`
- 实现: `core/ai/llm/anthropic_messages_client.cpp`
- 实现: `core/ai/llm/llm_client.h`
- 实现: `core/ai/llm/llm_chat_service.h`
- 实现: `core/ai/llm/llm_chat_service.cpp`
- 实现: `core/ai/llm/openai_compatible_client.h`
- 实现: `core/ai/llm/openai_compatible_client.cpp`
- 实现: `include/ai_types.h`
- 构建: `CMakeLists.txt`
- 测试: `tests/test_anthropic_messages_client.cpp`

**测试用例**（按被测方法分组，每方法 ≥ 1 条 happy path）:

SseEventParser.feed:
- `feed_whenEventIsSplitAcrossChunks_shouldPublishOneCompleteEvent`（happy path）
- `feed_whenDataHasMultipleLinesAndCrLf_shouldJoinDataInOrder`
- `feed_whenMalformedFieldIsReceived_shouldReturnSafeFramingError`

SseEventParser.finish:
- `finish_whenFinalEventHasNoBlankLine_shouldPublishCompleteEvent`（happy path）
- `finish_whenFinalLineIsIncomplete_shouldReturnFramingError`

AnthropicMessagesClient.sendChatCompletionStreamAsync:
- `sendChatCompletionStreamAsync_whenTextStreamCompletes_shouldPublishOrderedDeltasAndUsage`（happy path）
- `sendChatCompletionStreamAsync_whenBaseUrlHasRootV1OrMessages_shouldNormalizeOneEndpoint`
- `sendChatCompletionStreamAsync_whenToolInputArrivesInFragments_shouldCompleteOneValidatedToolCall`
- `sendChatCompletionStreamAsync_whenThinkingAndSignatureArrive_shouldKeepThemOutOfVisibleEvents`
- `sendChatCompletionStreamAsync_whenContinuationHasTransportBlocks_shouldReplayExactAssistantBlocks`
- `sendChatCompletionStreamAsync_whenMessageStopIsMissing_shouldFailExactlyOnce`

LlmRequestHandle.cancel:
- `cancel_whenReplyIsActive_shouldAbortAndCompleteExactlyOnce`（happy path）
- `cancel_whenReplyAlreadyFinished_shouldBeIdempotent`

OpenAICompatibleClient.sendChatCompletionStreamAsync:
- `sendChatCompletionStreamAsync_whenLegacyCompletionSucceeds_shouldPublishOneDeltaThenComplete`（happy path）

---

## Task 2: 对话流式编排

- **关联设计**: §3.3 对话流式编排
- **状态**: [ ]
- **Wave**: 2
- **依赖**: Task 1

**文件**:
- 实现: `core/ai/model/model_router.h`
- 实现: `core/ai/model/model_router.cpp`
- 实现: `core/ai/llm/llm_chat_model_client.h`
- 实现: `core/ai/llm/llm_chat_model_client.cpp`
- 实现: `core/ai/ai_brain.h`
- 实现: `core/ai/ai_brain.cpp`
- 实现: `core/ai/ai_brain_loop.cpp`
- 实现: `core/ai/ai_brain_router.cpp`
- 构建: `CMakeLists.txt`
- 测试: `tests/test_streaming_dialogue.cpp`

**测试用例**（按被测方法分组，每方法 ≥ 1 条 happy path）:

ModelRouter.completeStreamAsync:
- `completeStreamAsync_whenPrimaryCompletes_shouldReturnPrimaryStream`（happy path）
- `completeStreamAsync_whenPrimaryFailsBeforeVisibleText_shouldUseFallbackWithoutLeakingPrimaryEvents`
- `completeStreamAsync_whenPrimaryFailsAfterVisibleText_shouldInterruptWithoutFallback`

AIBrain.triggerThink/thinkInternal:
- `triggerThink_whenStreamingReplyCompletes_shouldEmitOneLifecycleAndJoinedCompatibilityResponse`（happy path）
- `triggerThink_whenUserMessageIdProvided_shouldPreserveReplyToIdAcrossToolRounds`
- `thinkInternal_whenToolUseCompletes_shouldAppendContinuationToSameAssistantMessage`
- `tryHandleRoutedIntent_whenDirectReplySelected_shouldEmitNormalizedLifecycleWithoutNetwork`

AIBrain.stopCurrentResponse:
- `stopCurrentResponse_whenStreamIsActive_shouldKeepPartialTextAndFinishStoppedOnce`（happy path）
- `stopCurrentResponse_whenNoResponseIsActive_shouldBeNoOp`

---

## Task 3: 身份化聊天存储

- **关联设计**: §3.4 身份化聊天存储
- **状态**: [ ]
- **Wave**: 3
- **依赖**: Task 2

**文件**:
- 实现: `core/ai/chat/chat_types.h`
- 实现: `core/ai/chat/chat_types.cpp`
- 实现: `core/ai/chat/profile_chat_history_store.h`
- 实现: `core/ai/chat/profile_chat_history_store.cpp`
- 实现: `ui/chat_conversation_model.h`
- 实现: `ui/chat_conversation_model.cpp`
- 构建: `CMakeLists.txt`
- 测试: `tests/test_chat_history.cpp`
- 测试: `tests/test_chat_conversation_model.cpp`

**测试用例**（按被测方法分组，每方法 ≥ 1 条 happy path）:

ProfileChatHistoryStore.open/load:
- `openAndLoad_whenProfilesDiffer_shouldKeepHistoriesIsolated`（happy path）
- `openAndLoad_whenSingleProfileOwnsLegacyHistory_shouldImportOnceAndKeepLegacyFile`
- `load_whenLineIsCorruptOversizedOrPartial_shouldSkipItAndContinue`
- `load_whenReplyToIdIsMissingInLegacyEntry_shouldUseEmptyValue`

ProfileChatHistoryStore.appendFinal:
- `appendFinal_whenEntryIsValid_shouldRoundTripSchemaVersionTwoFields`（happy path）
- `appendFinal_whenAssistantHasReplyToId_shouldRoundTripReplyLink`
- `appendFinal_whenEntryIsNonTerminal_shouldRejectWithoutWriting`

ChatConversationModel.appendUserMessage:
- `appendUserMessage_whenTextIsValid_shouldInsertPersistAndReturnStableId`（happy path）
- `appendUserMessage_whenTextIsBlank_shouldRejectWithoutMutation`

ChatConversationModel.begin/append/finishAssistantMessage:
- `assistantLifecycle_whenDeltasComplete_shouldPersistOneJoinedTerminalEntry`（happy path）
- `assistantLifecycle_whenToolContinuationUsesSameId_shouldKeepReplyToIdAndOneEntry`
- `finishAssistantMessage_whenCalledTwice_shouldPersistOnlyOnce`

ChatConversationModel.markReadThrough:
- `markReadThrough_whenMessageExists_shouldRestoreProfileScopedMarker`（happy path）
- `markReadThrough_whenMessageIsUnknown_shouldLeaveMarkerUnchanged`

---

## Task 4: Fluent 完整聊天窗口

- **关联设计**: §3.5 Fluent 完整聊天窗口
- **状态**: [ ]
- **Wave**: 4
- **依赖**: Task 2、Task 3

**文件**:
- 实现: `ui/chat_history_window.h`
- 实现: `ui/chat_history_window.cpp`
- 实现: `ui/petwindow.h`
- 实现: `ui/petwindow.cpp`
- 实现: `ui/petwindow_ai.cpp`
- 构建: `CMakeLists.txt`
- 测试: `tests/test_chat_history_window.cpp`

**测试用例**（按被测方法分组，每方法 ≥ 1 条 happy path）:

ChatHistoryWindow.bindConversation:
- `bindConversation_whenModelHasMixedRoles_shouldRenderOrderedSelectableRowsWithoutConversationList`（happy path）
- `bindConversation_whenModelChangesOneMessage_shouldUpdateOnlyThatRow`

ChatHistoryWindow.revealConversation:
- `revealConversation_whenUnreadMessagesExist_shouldInsertOneVirtualLastReadDivider`（happy path）
- `revealConversation_whenSavedGeometryIsOffscreen_shouldClampToAvailableScreen`

ChatHistoryWindow streaming update:
- `streamingUpdate_whenViewportIsAtBottom_shouldFollowNewestDelta`（happy path）
- `streamingUpdate_whenUserHasScrolledUp_shouldPreserveScrollAndShowJumpButton`

GrowingPlainTextEdit.setHeightRange:
- `setHeightRange_whenDocumentGrows_shouldClampHeightAndUseInternalScroll`（happy path）

ChatHistoryWindow.messageSubmitted/stopRequested:
- `messageSubmitted_whenIdleAndEnterPressed_shouldEmitTrimmedTextAndClearInput`（happy path）
- `stopRequested_whenResponseIsActive_shouldKeepDraftAndEmitStop`

ChatHistoryWindow.retryRequested:
- `retryRequested_whenInterruptedReplyHasSourceUser_shouldEmitAssistantIdWithoutMutatingOldMessage`（happy path）
- `retryRequested_whenReplyToIdIsMissing_shouldHideRetryAction`

ChatHistoryWindow.lastFullyVisibleMessageId:
- `lastFullyVisibleMessageId_whenLastRowIsPartial_shouldReturnPreviousCompleteRow`（happy path）

---

## Task 5: 桌面快捷聊天与分段气泡

- **关联设计**: §3.6 桌面快捷聊天与分段气泡
- **状态**: [ ]
- **Wave**: 5
- **依赖**: Task 2、Task 3、Task 4

**文件**:
- 实现: `ui/streaming_text_paginator.h`
- 实现: `ui/streaming_text_paginator.cpp`
- 实现: `ui/bubble_playback_controller.h`
- 实现: `ui/bubble_playback_controller.cpp`
- 实现: `ui/thinking_status_selector.h`
- 实现: `ui/thinking_status_selector.cpp`
- 实现: `ui/liquidglasschatbubble.h`
- 实现: `ui/liquidglasschatbubble.cpp`
- 实现: `ui/liquidglasschatbubble_utils.cpp`
- 实现: `ui/petwindow.h`
- 实现: `ui/petwindow_bubble.cpp`
- 实现: `ui/petwindow_screen_chat.cpp`
- 构建: `CMakeLists.txt`
- 测试: `tests/test_streaming_text_paginator.cpp`
- 测试: `tests/test_bubble_playback_controller.cpp`

**测试用例**（按被测方法分组，每方法 ≥ 1 条 happy path）:

StreamingTextPaginator.feed:
- `feed_whenChineseSentencesAndParagraphsArriveIncrementally_shouldSealAtNaturalBoundaries`（happy path）
- `feed_whenDeltaIsEmpty_shouldNotCreateEmptyPage`

StreamingTextPaginator.feed/finish:
- `finish_whenUnpunctuatedUnicodeExceedsHardLimit_shouldPreserveGraphemesAndExactText`（happy path）

BubblePlaybackController.appendSealedPages/updateDraftPage:
- `updateDraftPage_whenViewerIsOnLatestPage_shouldUpdateDraftInPlace`（happy path）
- `updateDraftPage_whenViewerIsReviewingOldPage_shouldNotStealCurrentPage`

BubblePlaybackController.setHovered:
- `setHovered_whenAutoTimerIsRunning_shouldResumeFromRemainingDuration`（happy path）

BubblePlaybackController.previous/next/toggleUserPause:
- `nextWhenAutoPlaying_shouldAdvanceWithinBounds`（happy path）
- `previousWhenUserNavigates_shouldPauseUntilExplicitResume`
- `nextWhenAlreadyOnLastPage_shouldBeNoOp`

ThinkingStatusSelector.next:
- `next_whenStageChanges_shouldUseMatchingPresetPoolWithoutImmediateRepeat`（happy path）
- `next_whenRequestChanges_shouldResetPerRequestMemory`

LiquidGlassChatBubble.setDisplayedPage:
- `setDisplayedPage_whenPageCountChanges_shouldKeepTextAndFixedControlsNonOverlapping`（happy path）

---

## Task 6: Launcher 独立模型配置

- **关联设计**: §3.7 Launcher 独立模型配置
- **状态**: [ ]
- **Wave**: 6
- **依赖**: Task 1、Task 5（按设计执行顺序）

**文件**:
- 实现: `launcher/app_state.py`
- 实现: `launcher/config_loader.py`
- 实现: `launcher/pages/ai_page.py`
- 实现: `launcher/api_connection_tester.py`
- 实现: `core/configLoader/config_manager.h`
- 实现: `core/configLoader/config_manager.cpp`
- 实现: `include/ai_types.h`
- 配置: `config/default_common_config.example.json`
- 构建: `CMakeLists.txt`
- 测试: `tests/test_launcher_config.py`
- 测试: `tests/test_api_connection_tester.py`
- 测试: `tests/test_model_role_config.py`

**测试用例**（按被测方法分组，每方法 ≥ 1 条 happy path）:

AppState.from_config:
- `from_config_whenTextAndVisionUseDifferentEndpoints_shouldRestoreEachWithoutCrossCopy`（happy path）
- `from_config_whenOnlyLegacyFieldsExist_shouldRestoreCompatibleEndpointStates`

apply_model_service_settings:
- `apply_model_service_settings_whenServicesDiffer_shouldWriteIndependentFirstRoutesAndKeepFallbacks`（happy path）
- `apply_model_service_settings_whenRoleInheritsText_shouldMaterializeTextEndpoint`
- `apply_model_service_settings_whenOneRoleOverrides_shouldPreserveOtherRoleInheritance`
- `apply_model_service_settings_whenUnknownRouteFieldsExist_shouldPreserveThem`

ApiConnectionTester.test:
- `test_whenAnthropicEndpointIsValid_shouldBuildMessagesProbeAndReportSuccess`（happy path）
- `test_whenOpenAiCompatibleEndpointIsValid_shouldBuildChatCompletionsProbe`
- `test_whenProviderIsUnsupported_shouldFailBeforeNetwork`
- `test_whenProviderErrorContainsApiKey_shouldRedactAndTruncateMessage`

AiPage connection test:
- `connectionTest_whenTextAndVisionRunTogether_shouldMaintainIndependentPendingState`（happy path）
- `connectionTest_whenOldRequestFinishesLate_shouldIgnoreStaleResult`

ConfigManager.routeFromJson:
- `routeFromJson_whenAnthropicFieldsAreValid_shouldLoadVersionAndStringHeaders`（happy path）
- `routeFromJson_whenHeaderValueIsNotString_shouldIgnoreItWithoutPuttingItInExtraParams`
