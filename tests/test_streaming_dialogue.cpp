#include <QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTimer>

#include <memory>
#include <optional>
#include <atomic>

#include "ai/ai_brain.h"
#include "ai/ai_tool.h"
#include "ai/event/event_ledger.h"
#include "ai/model/model_role_registry.h"
#include "ai/model/model_router.h"
#include "ai/memory/sqlite_memory_repository.h"
#include "ai/runtime/agent_bootstrap.h"
#include "ai/runtime/agent_runtime_services.h"
#include "ai/runtime/runtime_ui_bridge.h"
#include "ai/tool_registry.h"

namespace {

ModelRouteConfig route(const QString& id) {
    ModelRouteConfig value;
    value.routeId = id;
    value.enabled = true;
    value.llm.enabled = true;
    value.llm.provider = QStringLiteral("anthropic-messages");
    value.llm.baseUrl = QStringLiteral("https://models.example/v1");
    value.llm.apiKey = QStringLiteral("test-key");
    value.llm.model = QStringLiteral("test-model");
    value.llm.timeoutMs = 1000;
    return value;
}

ModelRoleConfig dialogueRoutes(std::initializer_list<ModelRouteConfig> routes) {
    ModelRoleConfig value;
    value.role = ModelRole::Dialogue;
    value.routes = QList<ModelRouteConfig>(routes);
    return value;
}

class FakeRequestHandle final : public LlmRequestHandle {
public:
    void cancel() override { cancelled = true; }
    bool isCancelled() const override { return cancelled; }

    bool cancelled = false;
};

class FakeStreamingClient final : public ModelCompletionClient {
public:
    struct Attempt {
        QList<LlmStreamEvent> events;
        bool success = true;
        LlmResponse response;
        QString error;
        bool deferred = false;
        bool repeatCompletion = false;
    };

    struct Pending {
        LlmStreamObserver observer;
        LlmCompletionHandler completion;
        std::shared_ptr<FakeRequestHandle> handle;
        Attempt attempt;
    };

    QList<Attempt> attempts;
    QList<QString> routeIds;
    QList<QList<ChatMessage>> messageBatches;
    QList<std::shared_ptr<FakeRequestHandle>> handles;
    std::optional<Pending> pending;

    void completeOnce(const ModelRouteConfig&,
                      const QList<ChatMessage>&,
                      const QJsonArray&,
                      LlmCompletionHandler completion,
                      const QString&) override {
        completion(false, {}, QStringLiteral("legacy completion was not expected"));
    }

    std::shared_ptr<LlmRequestHandle> completeOnceStream(
        const ModelRouteConfig& selectedRoute,
        const QList<ChatMessage>& messages,
        const QJsonArray& tools,
        LlmStreamObserver observer,
        LlmCompletionHandler completion,
        const QString& petName) override {
        Q_UNUSED(tools)
        Q_UNUSED(petName)
        routeIds.append(selectedRoute.routeId);
        messageBatches.append(messages);
        auto handle = std::make_shared<FakeRequestHandle>();
        handles.append(handle);
        Attempt attempt;
        if (!attempts.isEmpty()) {
            attempt = attempts.takeFirst();
        } else {
            attempt.success = false;
            attempt.error = QStringLiteral("missing fake attempt");
        }
        if (attempt.deferred) {
            pending.emplace(Pending{std::move(observer), std::move(completion),
                                    handle, std::move(attempt)});
            return handle;
        }
        for (const LlmStreamEvent& event : attempt.events) {
            if (observer) observer(event);
        }
        const bool success = attempt.success;
        const LlmResponse response = attempt.response;
        const QString error = attempt.error;
        completion(success, response, error);
        if (attempt.repeatCompletion) completion(success, response, error);
        return handle;
    }

    void publishPending(const LlmStreamEvent& event) {
        QVERIFY(pending.has_value());
        if (pending->observer) pending->observer(event);
    }

    void finishPendingEvenIfCancelled() {
        QVERIFY(pending.has_value());
        Pending value = std::move(*pending);
        pending.reset();
        for (const LlmStreamEvent& event : value.attempt.events) {
            if (value.observer) value.observer(event);
        }
        value.completion(value.attempt.success, std::move(value.attempt.response),
                         std::move(value.attempt.error));
    }
};

class EchoTool final : public AITool {
public:
    EchoTool()
        : AITool(QStringLiteral("echo_value"), QStringLiteral("Echo a value"),
                 ToolCategory::Query) {}

    QJsonObject parameterSchema() const override {
        return {{QStringLiteral("type"), QStringLiteral("object")}};
    }

    ToolResult execute(const QJsonObject& params) override {
        return ToolResult::ok({{QStringLiteral("value"),
                                params.value(QStringLiteral("value"))}});
    }
};

class CurrentTimeTool final : public AITool {
public:
    CurrentTimeTool()
        : AITool(QStringLiteral("get_current_time"),
                 QStringLiteral("Return a deterministic time"),
                 ToolCategory::Query) {}

    QJsonObject parameterSchema() const override {
        return {{QStringLiteral("type"), QStringLiteral("object")}};
    }

    ToolResult execute(const QJsonObject&) override {
        return ToolResult::ok({{QStringLiteral("time"), QStringLiteral("12:00")}});
    }
};

class LaunchTool final : public AITool {
public:
    explicit LaunchTool(int* executions)
        : AITool(QStringLiteral("lx_music_launch"),
                 QStringLiteral("Launch a test application"),
                 ToolCategory::Action)
        , m_executions(executions) {}

    QJsonObject parameterSchema() const override {
        return {{QStringLiteral("type"), QStringLiteral("object")}};
    }

    ToolResult execute(const QJsonObject&) override {
        if (m_executions) ++(*m_executions);
        return ToolResult::ok();
    }

private:
    int* m_executions = nullptr;
};

bool initializeBrain(AIBrain& brain, QTemporaryDir& directory) {
    return directory.isValid()
        && brain.initializeStorage({directory.filePath(QStringLiteral("memory.db")),
                                    directory.filePath(QStringLiteral("memory.json"))}).isOk();
}

std::unique_ptr<CallbackRuntimeUiBridge> makeRuntimeBridge() {
    RuntimeUiCallbacks callbacks;
    callbacks.showChatBubble = [](const QString&, int) {};
    callbacks.notifyUser = [](const QString&, const QString&, int) {};
    return std::make_unique<CallbackRuntimeUiBridge>(
        std::move(callbacks),
        reinterpret_cast<AnimationPlayer*>(quintptr(1)),
        reinterpret_cast<AnimationManager*>(quintptr(2)));
}

RuntimeStartRequest runtimeRequestFor(QTemporaryDir& directory,
                                      AIBrain* brain,
                                      RuntimeUiBridge* bridge) {
    const QString profileId = QStringLiteral(
        "11111111-1111-4111-8111-111111111111");
    RuntimeStartRequest request;
    request.profile = {QStringLiteral("Milltina"), QStringLiteral("model.gltf"), profileId};
    request.profileMigration.profileId = profileId;
    request.profileMigration.registeredProfileIds = {profileId};
    request.profileMigration.appDataRoot = directory.path();
    request.profileMigration.legacyDatabasePath =
        directory.filePath(QStringLiteral("legacy-memory.db"));
    request.profileMigration.legacyJsonPath =
        directory.filePath(QStringLiteral("legacy-memory.json"));
    request.configHash = QString(64, QLatin1Char('a'));
    request.identityBaselineSchemaVersion = 1;
    request.identityBaselineHash = QString(64, QLatin1Char('b'));
    request.aiBrain = brain;
    request.uiBridge = bridge;
    return request;
}

QList<MemoryEntry> persistedMemories(const QString& databasePath) {
    SQLiteMemoryRepository repository;
    QString error;
    if (!repository.open(databasePath, &error)) return {};
    return repository.loadAll();
}

std::optional<MemoryEntry> persistedMemory(const QString& databasePath,
                                           const QString& id) {
    const QList<MemoryEntry> entries = persistedMemories(databasePath);
    const auto found = std::find_if(entries.cbegin(), entries.cend(),
                                    [&id](const MemoryEntry& entry) {
                                        return entry.id == id;
                                    });
    return found == entries.cend() ? std::nullopt
                                   : std::optional<MemoryEntry>(*found);
}

bool persistedMemoryContains(const QString& databasePath, const QString& text) {
    const QList<MemoryEntry> entries = persistedMemories(databasePath);
    return std::any_of(entries.cbegin(), entries.cend(),
                       [&text](const MemoryEntry& entry) {
                           return entry.summary.contains(text)
                               || entry.content.contains(text);
                       });
}

LlmStreamEvent delta(const QString& text) {
    return {LlmStreamEventType::TextDelta, QStringLiteral("provider-request"),
            ChatActivityStage::StreamingText, text};
}

LlmResponse textResponse(const QString& text) {
    LlmResponse response;
    response.content = text;
    return response;
}

} // namespace

class StreamingDialogueTests : public QObject {
    Q_OBJECT

private slots:
    void completeStreamAsync_whenPrimaryCompletes_shouldReturnPrimaryStream();
    void completeStreamAsync_whenPrimaryFailsBeforeVisibleText_shouldUseFallbackWithoutLeakingPrimaryEvents();
    void completeStreamAsync_whenPrimaryFailsAfterVisibleText_shouldInterruptWithoutFallback();
    void triggerThink_whenStreamingReplyCompletes_shouldEmitOneLifecycleAndJoinedCompatibilityResponse();
    void triggerThink_whenUserMessageIdProvided_shouldPreserveReplyToIdAcrossToolRounds();
    void thinkInternal_whenToolUseCompletes_shouldAppendContinuationToSameAssistantMessage();
    void triggerThink_afterToolRound_shouldNotLeakToolProtocolIntoNextRequest();
    void tryHandleRoutedIntent_whenDirectReplySelected_shouldEmitNormalizedLifecycleWithoutNetwork();
    void tryHandleRoutedIntent_whenDirectToolCallSelected_shouldEmitNormalizedLifecycleWithoutNetwork();
    void stopCurrentResponse_whenStreamIsActive_shouldKeepPartialTextAndFinishStoppedOnce();
    void stopCurrentResponse_whenProviderCompletesLate_shouldQueueResponseLogOnce();
    void stopCurrentResponse_whenToolConfirmationIsPending_shouldCancelConfirmationAndResolveNoOp();
    void stopCurrentResponse_whenNoResponseIsActive_shouldBeNoOp();
    void finishActiveResponse_whenProviderCompletesTwice_shouldEmitFinishedExactlyOnce();
    void finishActiveResponse_whenFinishedSlotStartsNextResponse_shouldPreserveNewLifecycle();
    void triggerThink_whenPreparationIsDelayed_shouldKeepGuiEventLoopResponsiveAndDispatchOnce();
    void triggerThink_whenStoppedBeforePreparationCompletes_shouldDiscardStaleResult();
    void triggerThink_whenPreparationFails_shouldFinishResponseAndClearBusyState();
    void triggerThink_whenLocalRouterHandlesRequest_shouldPreserveFastPath();
    void triggerThink_whenExplicitForgetNeedsLlm_shouldExcludeForgottenMemoryFromPrompt();
    void thinkInternal_whenRequestIsPrepared_shouldDispatchBeforeNonCriticalEffectsComplete();
    void thinkInternal_whenToolOutcomePersistenceIsDelayed_shouldDispatchNextRoundFirst();
    void finishActiveResponse_whenEventsAreQueued_shouldReflectOnlyAfterBarrier();
    void enqueueBarrier_whenGenerationIsStale_shouldNotMutateActiveResponse();
    void finishActiveResponse_whenToolRoundsComplete_shouldQueueEachResponseLogExactlyOnce();
    void stop_whenSideEffectsArePending_shouldNotDeliverCallbacksToDestroyedState();
    void messageSend_whenPreparationTakesOneHundredMilliseconds_shouldAllowSixteenMillisecondTimerToAdvance();
};

void StreamingDialogueTests::completeStreamAsync_whenPrimaryCompletes_shouldReturnPrimaryStream() {
    ModelRoleRegistry registry({dialogueRoutes({route(QStringLiteral("primary"))})});
    FakeStreamingClient client;
    client.attempts = {{{delta(QStringLiteral("hello"))}, true,
                        textResponse(QStringLiteral("hello")), {}, false}};
    ModelRouter router(&registry, &client);
    ModelRequest request;
    request.role = ModelRole::Dialogue;
    QString visible;
    std::optional<Result<ModelCompletion, DomainError>> result;

    const auto handle = router.completeStreamAsync(
        request,
        [&visible](const LlmStreamEvent& event) { visible += event.textDelta; },
        [&result](Result<ModelCompletion, DomainError> value) {
            result.emplace(std::move(value));
        });

    QVERIFY(handle);
    QCOMPARE(visible, QStringLiteral("hello"));
    QVERIFY(result.has_value() && result->isOk());
    QCOMPARE(result->value().dimensions.routeId, QStringLiteral("primary"));
    QVERIFY(!result->value().fallbackUsed);
}

void StreamingDialogueTests::completeStreamAsync_whenPrimaryFailsBeforeVisibleText_shouldUseFallbackWithoutLeakingPrimaryEvents() {
    ModelRoleRegistry registry({dialogueRoutes(
        {route(QStringLiteral("primary")), route(QStringLiteral("fallback"))})});
    FakeStreamingClient client;
    client.attempts = {
        {{{LlmStreamEventType::Started, QStringLiteral("failed-request"),
           ChatActivityStage::WaitingForModel, {}}},
         false, {}, QStringLiteral("network timeout"), false},
        {{{LlmStreamEventType::Started, QStringLiteral("fallback-request"),
           ChatActivityStage::WaitingForModel, {}}, delta(QStringLiteral("fallback"))},
         true, textResponse(QStringLiteral("fallback")), {}, false}
    };
    ModelRouter router(&registry, &client);
    ModelRequest request;
    request.role = ModelRole::Dialogue;
    QList<LlmStreamEvent> published;
    std::optional<Result<ModelCompletion, DomainError>> result;

    router.completeStreamAsync(
        request,
        [&published](const LlmStreamEvent& event) { published.append(event); },
        [&result](Result<ModelCompletion, DomainError> value) {
            result.emplace(std::move(value));
        });

    QVERIFY(result.has_value() && result->isOk());
    QCOMPARE(client.routeIds,
             QList<QString>({QStringLiteral("primary"), QStringLiteral("fallback")}));
    QCOMPARE(published.size(), 2);
    QCOMPARE(published.first().requestId, QStringLiteral("fallback-request"));
    QCOMPARE(published.last().textDelta, QStringLiteral("fallback"));
    QVERIFY(result->value().fallbackUsed);
}

void StreamingDialogueTests::completeStreamAsync_whenPrimaryFailsAfterVisibleText_shouldInterruptWithoutFallback() {
    ModelRoleRegistry registry({dialogueRoutes(
        {route(QStringLiteral("primary")), route(QStringLiteral("fallback"))})});
    FakeStreamingClient client;
    client.attempts = {
        {{delta(QStringLiteral("partial"))}, false, {},
         QStringLiteral("network disconnected"), false},
        {{delta(QStringLiteral("must-not-run"))}, true,
         textResponse(QStringLiteral("must-not-run")), {}, false}
    };
    ModelRouter router(&registry, &client);
    ModelRequest request;
    request.role = ModelRole::Dialogue;
    QString visible;
    std::optional<Result<ModelCompletion, DomainError>> result;

    router.completeStreamAsync(
        request,
        [&visible](const LlmStreamEvent& event) { visible += event.textDelta; },
        [&result](Result<ModelCompletion, DomainError> value) {
            result.emplace(std::move(value));
        });

    QCOMPARE(visible, QStringLiteral("partial"));
    QCOMPARE(client.routeIds, QList<QString>({QStringLiteral("primary")}));
    QVERIFY(result.has_value() && !result->isOk());
    QCOMPARE(result->error().code, QStringLiteral("MODEL_STREAM_INTERRUPTED"));
}

void StreamingDialogueTests::triggerThink_whenStreamingReplyCompletes_shouldEmitOneLifecycleAndJoinedCompatibilityResponse() {
    FakeStreamingClient client;
    client.attempts = {{{delta(QStringLiteral("你")), delta(QStringLiteral("好"))},
                        true, textResponse(QStringLiteral("你好")), {}, false}};
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(initializeBrain(brain, directory));
    QStringList startedIds;
    QStringList deltaIds;
    QString joined;
    QList<ChatMessageStatus> statuses;
    QStringList compatibility;
    connect(&brain, &AIBrain::assistantResponseStarted, this,
            [&startedIds](const QString& id, const QString&, const QString&) {
                startedIds.append(id);
            });
    connect(&brain, &AIBrain::assistantResponseDelta, this,
            [&deltaIds, &joined](const QString& id, const QString& text) {
                deltaIds.append(id);
                joined += text;
            });
    connect(&brain, &AIBrain::assistantResponseFinished, this,
            [&statuses](const QString&, ChatMessageStatus status, const QString&) {
                statuses.append(status);
            });
    connect(&brain, &AIBrain::assistantResponseReady, this,
            [&compatibility](const QString& text) { compatibility.append(text); });

    brain.triggerThink(QStringLiteral("请回答一个复杂问题"),
                       QStringLiteral("user_request"), QStringLiteral("user-1"));

    QTRY_COMPARE_WITH_TIMEOUT(statuses.size(), 1, 2000);
    QCOMPARE(startedIds.size(), 1);
    QCOMPARE(joined, QStringLiteral("你好"));
    QCOMPARE(deltaIds, QList<QString>({startedIds.first(), startedIds.first()}));
    QCOMPARE(statuses, QList<ChatMessageStatus>({ChatMessageStatus::Complete}));
    QCOMPARE(compatibility, QStringList({QStringLiteral("你好")}));
    QVERIFY(!brain.isBusy());
}

void StreamingDialogueTests::triggerThink_whenUserMessageIdProvided_shouldPreserveReplyToIdAcrossToolRounds() {
    FakeStreamingClient client;
    LlmResponse toolResponse = textResponse(QStringLiteral("先查一下"));
    toolResponse.toolCalls.append({QStringLiteral("tool-1"), QStringLiteral("function"),
                                   QStringLiteral("echo_value"),
                                   {{QStringLiteral("value"), 7}}});
    toolResponse.transportBlocks = {QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_use")},
                                                {QStringLiteral("id"), QStringLiteral("tool-1")}}};
    client.attempts = {
        {{delta(QStringLiteral("先查一下"))}, true, toolResponse, {}, false},
        {{delta(QStringLiteral("查完了"))}, true, textResponse(QStringLiteral("查完了")), {}, false}
    };
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(initializeBrain(brain, directory));
    ToolRegistry tools;
    tools.registerTool(std::make_unique<EchoTool>());
    brain.setToolRegistry(&tools);
    QStringList replyIds;
    QStringList messageIds;
    connect(&brain, &AIBrain::assistantResponseStarted, this,
            [&replyIds, &messageIds](const QString& id, const QString& replyTo,
                                    const QString&) {
                messageIds.append(id);
                replyIds.append(replyTo);
            });

    brain.triggerThink(QStringLiteral("分析并使用工具"),
                       QStringLiteral("user_request"), QStringLiteral("user-source"));

    QTRY_COMPARE_WITH_TIMEOUT(client.routeIds.size(), 2, 2000);
    QCOMPARE(client.routeIds.size(), 2);
    QCOMPARE(messageIds.size(), 1);
    QCOMPARE(replyIds, QStringList({QStringLiteral("user-source")}));
}

void StreamingDialogueTests::thinkInternal_whenToolUseCompletes_shouldAppendContinuationToSameAssistantMessage() {
    FakeStreamingClient client;
    LlmResponse toolResponse = textResponse(QStringLiteral("before"));
    toolResponse.toolCalls.append({QStringLiteral("tool-1"), QStringLiteral("function"),
                                   QStringLiteral("echo_value"), {}});
    const QJsonArray transportBlocks{
        QJsonObject{{QStringLiteral("type"), QStringLiteral("thinking")},
                    {QStringLiteral("signature"), QStringLiteral("opaque")}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_use")},
                    {QStringLiteral("id"), QStringLiteral("tool-1")}}
    };
    toolResponse.transportBlocks = transportBlocks;
    client.attempts = {
        {{delta(QStringLiteral("before"))}, true, toolResponse, {}, false},
        {{delta(QStringLiteral("after"))}, true, textResponse(QStringLiteral("after")), {}, false}
    };
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(initializeBrain(brain, directory));
    ToolRegistry tools;
    tools.registerTool(std::make_unique<EchoTool>());
    brain.setToolRegistry(&tools);
    QStringList deltaMessageIds;
    QString visible;
    QList<ChatActivityStage> stages;
    connect(&brain, &AIBrain::assistantResponseDelta, this,
            [&deltaMessageIds, &visible](const QString& id, const QString& text) {
                deltaMessageIds.append(id);
                visible += text;
            });
    connect(&brain, &AIBrain::assistantResponseStageChanged, this,
            [&stages](const QString&, ChatActivityStage stage) {
                stages.append(stage);
            });

    brain.triggerThink(QStringLiteral("use the echo tool"),
                       QStringLiteral("user_request"), QStringLiteral("user-2"));

    QTRY_COMPARE_WITH_TIMEOUT(client.messageBatches.size(), 2, 2000);
    QCOMPARE(client.messageBatches.size(), 2);
    const QList<ChatMessage>& continuation = client.messageBatches.at(1);
    const auto assistant = std::find_if(
        continuation.cbegin(), continuation.cend(), [](const ChatMessage& message) {
            return message.role == QLatin1String("assistant")
                && !message.transportBlocks.isEmpty();
        });
    QVERIFY(assistant != continuation.cend());
    QCOMPARE(assistant->transportBlocks, transportBlocks);
    QCOMPARE(visible, QStringLiteral("beforeafter"));
    QVERIFY(!deltaMessageIds.isEmpty());
    for (const QString& id : deltaMessageIds) {
        QCOMPARE(id, deltaMessageIds.first());
    }
    QCOMPARE(stages, QList<ChatActivityStage>({
        ChatActivityStage::WaitingForModel,
        ChatActivityStage::StreamingText,
        ChatActivityStage::PreparingTool,
        ChatActivityStage::RunningTool,
        ChatActivityStage::Finalizing,
        ChatActivityStage::StreamingText,
        ChatActivityStage::Finalizing
    }));
}

void StreamingDialogueTests::triggerThink_afterToolRound_shouldNotLeakToolProtocolIntoNextRequest() {
    FakeStreamingClient client;
    LlmResponse toolResponse;
    toolResponse.toolCalls.append({QStringLiteral("tool-1"), QStringLiteral("function"),
                                   QStringLiteral("echo_value"), {}});
    client.attempts = {
        {{}, true, toolResponse, {}, false},
        {{delta(QStringLiteral("第一次完成"))}, true,
         textResponse(QStringLiteral("第一次完成")), {}, false},
        {{delta(QStringLiteral("第二次完成"))}, true,
         textResponse(QStringLiteral("第二次完成")), {}, false}
    };
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(initializeBrain(brain, directory));
    ToolRegistry tools;
    tools.registerTool(std::make_unique<EchoTool>());
    brain.setToolRegistry(&tools);

    brain.triggerThink(QStringLiteral("第一次请求"),
                       QStringLiteral("user_request"), QStringLiteral("user-1"));
    QTRY_COMPARE_WITH_TIMEOUT(client.messageBatches.size(), 2, 2000);
    brain.triggerThink(QStringLiteral("第二次请求"),
                       QStringLiteral("user_request"), QStringLiteral("user-2"));

    QTRY_COMPARE_WITH_TIMEOUT(client.messageBatches.size(), 3, 2000);
    QCOMPARE(client.messageBatches.size(), 3);
    const QList<ChatMessage>& secondRequest = client.messageBatches.at(2);
    QVERIFY(std::none_of(
        secondRequest.cbegin(), secondRequest.cend(), [](const ChatMessage& message) {
            return message.role == QLatin1String("tool")
                || !message.toolCallId.isEmpty() || !message.toolCalls.isEmpty();
        }));
}

void StreamingDialogueTests::tryHandleRoutedIntent_whenDirectReplySelected_shouldEmitNormalizedLifecycleWithoutNetwork() {
    FakeStreamingClient client;
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(initializeBrain(brain, directory));
    int startedCount = 0;
    QString deltaText;
    QList<ChatMessageStatus> statuses;
    connect(&brain, &AIBrain::assistantResponseStarted, this,
            [&startedCount](const QString&, const QString&, const QString&) {
                ++startedCount;
            });
    connect(&brain, &AIBrain::assistantResponseDelta, this,
            [&deltaText](const QString&, const QString& text) { deltaText += text; });
    connect(&brain, &AIBrain::assistantResponseFinished, this,
            [&statuses](const QString&, ChatMessageStatus status, const QString&) {
                statuses.append(status);
            });

    brain.triggerThink(QStringLiteral("你好"), QStringLiteral("user_request"),
                       QStringLiteral("user-greeting"));

    QCOMPARE(client.routeIds.size(), 0);
    QCOMPARE(startedCount, 1);
    QCOMPARE(deltaText, QStringLiteral("在哦。"));
    QCOMPARE(statuses, QList<ChatMessageStatus>({ChatMessageStatus::Complete}));
}

void StreamingDialogueTests::tryHandleRoutedIntent_whenDirectToolCallSelected_shouldEmitNormalizedLifecycleWithoutNetwork() {
    FakeStreamingClient client;
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(initializeBrain(brain, directory));
    ToolRegistry tools;
    tools.registerTool(std::make_unique<CurrentTimeTool>());
    brain.setToolRegistry(&tools);
    int startedCount = 0;
    QString visible;
    QList<ChatMessageStatus> statuses;
    QList<ChatActivityStage> stages;
    connect(&brain, &AIBrain::assistantResponseStarted, this,
            [&startedCount](const QString&, const QString&, const QString&) {
                ++startedCount;
            });
    connect(&brain, &AIBrain::assistantResponseDelta, this,
            [&visible](const QString&, const QString& text) { visible += text; });
    connect(&brain, &AIBrain::assistantResponseFinished, this,
            [&statuses](const QString&, ChatMessageStatus status, const QString&) {
                statuses.append(status);
            });
    connect(&brain, &AIBrain::assistantResponseStageChanged, this,
            [&stages](const QString&, ChatActivityStage stage) {
                stages.append(stage);
            });

    brain.triggerThink(QStringLiteral("现在几点"), QStringLiteral("user_request"),
                       QStringLiteral("user-time"));

    QCOMPARE(client.routeIds.size(), 0);
    QCOMPARE(startedCount, 1);
    QCOMPARE(visible, QStringLiteral("现在是 12:00。"));
    QCOMPARE(statuses, QList<ChatMessageStatus>({ChatMessageStatus::Complete}));
    QCOMPARE(stages, QList<ChatActivityStage>({
        ChatActivityStage::WaitingForModel,
        ChatActivityStage::PreparingTool,
        ChatActivityStage::RunningTool,
        ChatActivityStage::Finalizing,
        ChatActivityStage::StreamingText,
        ChatActivityStage::Finalizing
    }));
}

void StreamingDialogueTests::stopCurrentResponse_whenStreamIsActive_shouldKeepPartialTextAndFinishStoppedOnce() {
    FakeStreamingClient client;
    FakeStreamingClient::Attempt attempt;
    attempt.deferred = true;
    attempt.success = true;
    attempt.response = textResponse(QStringLiteral("partial-late"));
    attempt.events = {delta(QStringLiteral("-late"))};
    client.attempts = {attempt};
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(initializeBrain(brain, directory));
    QString visible;
    QList<ChatMessageStatus> statuses;
    QStringList compatibility;
    connect(&brain, &AIBrain::assistantResponseDelta, this,
            [&visible](const QString&, const QString& text) { visible += text; });
    connect(&brain, &AIBrain::assistantResponseFinished, this,
            [&statuses](const QString&, ChatMessageStatus status, const QString&) {
                statuses.append(status);
            });
    connect(&brain, &AIBrain::assistantResponseReady, this,
            [&compatibility](const QString& text) { compatibility.append(text); });

    brain.triggerThink(QStringLiteral("long network answer"),
                       QStringLiteral("user_request"), QStringLiteral("user-stop"));
    QTRY_VERIFY_WITH_TIMEOUT(client.pending.has_value(), 2000);
    client.publishPending(delta(QStringLiteral("partial")));
    brain.stopCurrentResponse();
    client.finishPendingEvenIfCancelled();

    QCOMPARE(visible, QStringLiteral("partial"));
    QCOMPARE(statuses, QList<ChatMessageStatus>({ChatMessageStatus::Stopped}));
    QCOMPARE(compatibility.size(), 0);
    QVERIFY(client.handles.first()->isCancelled());
    QVERIFY(!brain.isBusy());
}

void StreamingDialogueTests::
stopCurrentResponse_whenProviderCompletesLate_shouldQueueResponseLogOnce() {
    FakeStreamingClient client;
    FakeStreamingClient::Attempt attempt;
    attempt.deferred = true;
    attempt.success = true;
    attempt.response = textResponse(QStringLiteral("late"));
    client.attempts = {attempt};
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto bridge = makeRuntimeBridge();
    AgentRuntimeServices services;
    QVERIFY(AgentBootstrap::start(
        services, runtimeRequestFor(directory, &brain, bridge.get())).isOk());
    std::atomic_int responseLogs{0};
    brain.setChatSideEffectLifecycleProbeForTests(
        [&responseLogs](const QString& phase, const QString&, quintptr) {
            if (phase == QLatin1String("response.log.completed")) ++responseLogs;
        });

    brain.triggerThink(QStringLiteral("cancel before late completion"),
                       QStringLiteral("user_request"), QStringLiteral("late-log"));
    QTRY_VERIFY_WITH_TIMEOUT(client.pending.has_value(), 2000);
    brain.stopCurrentResponse();
    client.finishPendingEvenIfCancelled();

    QTRY_COMPARE_WITH_TIMEOUT(responseLogs.load(), 1, 2000);
    QCOMPARE(responseLogs.load(), 1);
}

void StreamingDialogueTests::stopCurrentResponse_whenNoResponseIsActive_shouldBeNoOp() {
    FakeStreamingClient client;
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    int finishCount = 0;
    connect(&brain, &AIBrain::assistantResponseFinished, this,
            [&finishCount](const QString&, ChatMessageStatus, const QString&) {
                ++finishCount;
            });

    brain.stopCurrentResponse();

    QCOMPARE(finishCount, 0);
    QVERIFY(!brain.isBusy());
}

void StreamingDialogueTests::stopCurrentResponse_whenToolConfirmationIsPending_shouldCancelConfirmationAndResolveNoOp() {
    FakeStreamingClient client;
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(initializeBrain(brain, directory));
    int executions = 0;
    ToolRegistry tools;
    tools.registerTool(std::make_unique<LaunchTool>(&executions));
    brain.setToolRegistry(&tools);
    QString confirmationId;
    QList<ChatMessageStatus> statuses;
    connect(&brain, &AIBrain::toolConfirmationRequired, this,
            [&confirmationId](const QString& id, const QString&, const QString&,
                              const QJsonObject&) {
                confirmationId = id;
            });
    connect(&brain, &AIBrain::assistantResponseFinished, this,
            [&statuses](const QString&, ChatMessageStatus status, const QString&) {
                statuses.append(status);
            });

    brain.triggerThink(QStringLiteral("启动lx"), QStringLiteral("user_request"),
                       QStringLiteral("user-confirmation"));
    QVERIFY(!confirmationId.isEmpty());
    QVERIFY(brain.isBusy());
    brain.stopCurrentResponse();
    brain.resolveToolConfirmation(confirmationId, true);

    QCOMPARE(executions, 0);
    QCOMPARE(statuses, QList<ChatMessageStatus>({ChatMessageStatus::Stopped}));
    QVERIFY(!brain.isBusy());
}

void StreamingDialogueTests::finishActiveResponse_whenProviderCompletesTwice_shouldEmitFinishedExactlyOnce() {
    FakeStreamingClient client;
    FakeStreamingClient::Attempt attempt;
    attempt.events = {delta(QStringLiteral("once"))};
    attempt.response = textResponse(QStringLiteral("once"));
    attempt.repeatCompletion = true;
    client.attempts = {attempt};
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto bridge = makeRuntimeBridge();
    AgentRuntimeServices services;
    QVERIFY(AgentBootstrap::start(
        services, runtimeRequestFor(directory, &brain, bridge.get())).isOk());
    int finishCount = 0;
    int compatibilityCount = 0;
    std::atomic_int responseLogs{0};
    brain.setChatSideEffectLifecycleProbeForTests(
        [&responseLogs](const QString& phase, const QString&, quintptr) {
            if (phase == QLatin1String("response.log.completed")) ++responseLogs;
        });
    connect(&brain, &AIBrain::assistantResponseFinished, this,
            [&finishCount](const QString&, ChatMessageStatus, const QString&) {
                ++finishCount;
            });
    connect(&brain, &AIBrain::assistantResponseReady, this,
            [&compatibilityCount](const QString&) { ++compatibilityCount; });

    brain.triggerThink(QStringLiteral("duplicate provider callback"),
                       QStringLiteral("user_request"), QStringLiteral("user-dup"));

    QTRY_COMPARE_WITH_TIMEOUT(finishCount, 1, 2000);
    QCOMPARE(finishCount, 1);
    QCOMPARE(compatibilityCount, 1);
    QTRY_COMPARE_WITH_TIMEOUT(responseLogs.load(), 1, 2000);
}

void StreamingDialogueTests::finishActiveResponse_whenFinishedSlotStartsNextResponse_shouldPreserveNewLifecycle() {
    FakeStreamingClient client;
    FakeStreamingClient::Attempt second;
    second.deferred = true;
    second.response = textResponse(QStringLiteral("second"));
    client.attempts = {
        {{delta(QStringLiteral("first"))}, true,
         textResponse(QStringLiteral("first")), {}, false},
        second
    };
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(initializeBrain(brain, directory));
    QStringList startedIds;
    QStringList finishedIds;
    QStringList visible;
    connect(&brain, &AIBrain::assistantResponseStarted, this,
            [&startedIds](const QString& id, const QString&, const QString&) {
                startedIds.append(id);
            });
    connect(&brain, &AIBrain::assistantResponseDelta, this,
            [&visible](const QString&, const QString& text) { visible.append(text); });
    connect(&brain, &AIBrain::assistantResponseFinished, this,
            [&brain, &finishedIds](const QString& id, ChatMessageStatus,
                                   const QString&) {
                finishedIds.append(id);
                if (finishedIds.size() == 1) {
                    brain.triggerThink(QStringLiteral("second complex request"),
                                       QStringLiteral("user_request"),
                                       QStringLiteral("user-second"));
                }
            });

    brain.triggerThink(QStringLiteral("first complex request"),
                       QStringLiteral("user_request"), QStringLiteral("user-first"));
    QTRY_VERIFY_WITH_TIMEOUT(client.pending.has_value(), 2000);
    client.publishPending(delta(QStringLiteral("second")));
    client.finishPendingEvenIfCancelled();

    QCOMPARE(startedIds.size(), 2);
    QVERIFY(startedIds.first() != startedIds.last());
    QCOMPARE(finishedIds, startedIds);
    QCOMPARE(visible, QStringList({QStringLiteral("first"),
                                   QStringLiteral("second")}));
    QVERIFY(!brain.isBusy());
}

void StreamingDialogueTests::
triggerThink_whenPreparationIsDelayed_shouldKeepGuiEventLoopResponsiveAndDispatchOnce() {
    FakeStreamingClient client;
    client.attempts = {{{delta(QStringLiteral("done"))}, true,
                        textResponse(QStringLiteral("done")), {}, false}};
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(initializeBrain(brain, directory));
    brain.setChatPreparationDelayForTests(100);
    int timerTicks = 0;
    bool timerAdvancedBeforeDispatch = false;
    QTimer frameTimer;
    frameTimer.setInterval(16);
    connect(&frameTimer, &QTimer::timeout, this,
            [&client, &timerTicks, &timerAdvancedBeforeDispatch]() {
                ++timerTicks;
                if (client.routeIds.isEmpty()) timerAdvancedBeforeDispatch = true;
            });
    frameTimer.start();

    brain.triggerThink(QStringLiteral("需要模型处理的复杂问题"),
                       QStringLiteral("user_request"), QStringLiteral("user-async"));

    QCOMPARE(client.routeIds.size(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(client.routeIds.size(), 1, 2000);
    frameTimer.stop();
    QCOMPARE(client.routeIds.size(), 1);
    QVERIFY(timerAdvancedBeforeDispatch);
    QVERIFY(timerTicks >= 3);
    QVERIFY(!brain.isBusy());
}

void StreamingDialogueTests::
triggerThink_whenStoppedBeforePreparationCompletes_shouldDiscardStaleResult() {
    FakeStreamingClient client;
    client.attempts = {{{delta(QStringLiteral("must-not-run"))}, true,
                        textResponse(QStringLiteral("must-not-run")), {}, false}};
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(initializeBrain(brain, directory));
    brain.setChatPreparationDelayForTests(100);
    QList<ChatMessageStatus> statuses;
    connect(&brain, &AIBrain::assistantResponseFinished, this,
            [&statuses](const QString&, ChatMessageStatus status, const QString&) {
                statuses.append(status);
            });

    brain.triggerThink(QStringLiteral("需要模型处理并允许立刻取消"),
                       QStringLiteral("user_request"), QStringLiteral("user-stop-prep"));
    QTest::qWait(10);
    brain.stopCurrentResponse();
    QTest::qWait(140);

    QCOMPARE(client.routeIds.size(), 0);
    QCOMPARE(statuses, QList<ChatMessageStatus>({ChatMessageStatus::Stopped}));
    QVERIFY(!brain.isBusy());
}

void StreamingDialogueTests::
triggerThink_whenPreparationFails_shouldFinishResponseAndClearBusyState() {
    FakeStreamingClient client;
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(initializeBrain(brain, directory));
    brain.stop();
    QList<ChatMessageStatus> statuses;
    connect(&brain, &AIBrain::assistantResponseFinished, this,
            [&statuses](const QString&, ChatMessageStatus status, const QString&) {
                statuses.append(status);
            });

    brain.triggerThink(QStringLiteral("准备执行器已停止后的复杂请求"),
                       QStringLiteral("user_request"), QStringLiteral("user-prep-fail"));

    QTRY_COMPARE_WITH_TIMEOUT(statuses.size(), 1, 1000);
    QCOMPARE(statuses.first(), ChatMessageStatus::Failed);
    QCOMPARE(client.routeIds.size(), 0);
    QVERIFY(!brain.isBusy());
}

void StreamingDialogueTests::
triggerThink_whenLocalRouterHandlesRequest_shouldPreserveFastPath() {
    FakeStreamingClient client;
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto bridge = makeRuntimeBridge();
    AgentRuntimeServices services;
    const RuntimeStartRequest request = runtimeRequestFor(
        directory, &brain, bridge.get());
    const auto started = AgentBootstrap::start(services, request);
    QVERIFY(started.isOk());
    ToolRegistry tools;
    tools.registerTool(std::make_unique<CurrentTimeTool>());
    brain.setToolRegistry(&tools);
    QList<ChatMessageStatus> statuses;
    connect(&brain, &AIBrain::assistantResponseFinished, this,
            [&statuses](const QString&, ChatMessageStatus status, const QString&) {
                statuses.append(status);
            });

    brain.triggerThink(QStringLiteral("我喜欢爵士乐，现在几点"),
                       QStringLiteral("user_request"),
                       QStringLiteral("user-fast"));

    QCOMPARE(statuses, QList<ChatMessageStatus>({ChatMessageStatus::Complete}));
    QCOMPARE(client.routeIds.size(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(persistedMemoryContains(
        brain.memoryStore()->databasePath(), QStringLiteral("爵士乐")), 2000);
    const auto authorization = services.authorizationFor(QStringLiteral("identity"));
    QVERIFY(authorization.isOk());
    EventFilter filter{{QStringLiteral("UserMessageReceived")},
                       QString(), authorization.value()};
    QTRY_VERIFY_WITH_TIMEOUT(([&services, &filter]() {
        const auto events = services.eventLedger()->readAfter(0, filter, 20);
        return events.isOk() && events.value().size() == 1;
    }()), 2000);
    QVERIFY(!brain.isBusy());
}

void StreamingDialogueTests::
triggerThink_whenExplicitForgetNeedsLlm_shouldExcludeForgottenMemoryFromPrompt() {
    FakeStreamingClient client;
    client.attempts = {{{delta(QStringLiteral("已忘记"))}, true,
                        textResponse(QStringLiteral("已忘记")), {}, false}};
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto bridge = makeRuntimeBridge();
    AgentRuntimeServices services;
    QVERIFY(AgentBootstrap::start(
        services, runtimeRequestFor(directory, &brain, bridge.get())).isOk());
    MemoryEntry memory;
    memory.id = QStringLiteral("forget-jazz-memory");
    memory.type = MemoryType::Preference;
    memory.status = MemoryStatus::Active;
    memory.key = QStringLiteral("preference:jazz");
    memory.summary = QStringLiteral("主人喜欢爵士乐 OLD_MEMORY_SENTINEL");
    memory.content = memory.summary;
    memory.importance = 0.9;
    memory.strength = 0.8;
    memory.confidence = 0.9;
    memory.createdAt = QDateTime::currentDateTimeUtc();
    memory.updatedAt = memory.createdAt;
    const MemoryEntry stored = brain.memoryStore()->addEntry(memory);
    QCOMPARE(stored.status, MemoryStatus::Active);
    brain.setChatSideEffectDelayForTests(100);

    brain.triggerThink(QStringLiteral("忘记爵士乐"),
                       QStringLiteral("user_request"),
                       QStringLiteral("user-forget"));

    QTRY_COMPARE_WITH_TIMEOUT(client.messageBatches.size(), 1, 2000);
    for (const ChatMessage& message : client.messageBatches.first()) {
        QVERIFY2(!message.content.contains(QStringLiteral("OLD_MEMORY_SENTINEL")),
                 qPrintable(message.content));
    }
    const MemoryEntry* cachedForgotten = brain.memoryStore()->findById(stored.id);
    QVERIFY(cachedForgotten);
    QCOMPARE(cachedForgotten->status, MemoryStatus::Deleted);
    const std::optional<MemoryEntry> beforeCommit = persistedMemory(
        brain.memoryStore()->databasePath(), stored.id);
    QVERIFY(beforeCommit.has_value());
    QCOMPARE(beforeCommit->status, MemoryStatus::Active);
    QTRY_VERIFY_WITH_TIMEOUT(([&brain, &stored]() {
        const auto forgotten = persistedMemory(brain.memoryStore()->databasePath(),
                                                stored.id);
        return forgotten.has_value()
            && forgotten->status == MemoryStatus::Deleted;
    }()), 2000);
    QVERIFY(!brain.isBusy());
}

void StreamingDialogueTests::
thinkInternal_whenRequestIsPrepared_shouldDispatchBeforeNonCriticalEffectsComplete() {
    FakeStreamingClient client;
    FakeStreamingClient::Attempt attempt;
    attempt.deferred = true;
    attempt.success = true;
    attempt.response = textResponse(QStringLiteral("done"));
    client.attempts = {attempt};
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto bridge = makeRuntimeBridge();
    AgentRuntimeServices services;
    QVERIFY(AgentBootstrap::start(
        services, runtimeRequestFor(directory, &brain, bridge.get())).isOk());
    std::atomic_int completedEffects{0};
    brain.setChatSideEffectDelayForTests(150);
    brain.setChatSideEffectLifecycleProbeForTests(
        [&completedEffects](const QString& phase, const QString&, quintptr) {
            if (phase == QLatin1String("effect.completed")) ++completedEffects;
        });

    brain.triggerThink(QStringLiteral("请记住 SIDE_EFFECT_MEMORY_SENTINEL"),
                       QStringLiteral("user_request"),
                       QStringLiteral("dispatch-before-effects"));

    QTRY_VERIFY_WITH_TIMEOUT(client.pending.has_value(), 2000);
    QCOMPARE(client.routeIds.size(), 1);
    QCOMPARE(completedEffects.load(), 0);
    QVERIFY(!persistedMemoryContains(
        brain.memoryStore()->databasePath(),
        QStringLiteral("SIDE_EFFECT_MEMORY_SENTINEL")));
    client.finishPendingEvenIfCancelled();
    QTRY_VERIFY_WITH_TIMEOUT(persistedMemoryContains(
        brain.memoryStore()->databasePath(),
        QStringLiteral("SIDE_EFFECT_MEMORY_SENTINEL")), 3000);
}

void StreamingDialogueTests::
thinkInternal_whenToolOutcomePersistenceIsDelayed_shouldDispatchNextRoundFirst() {
    FakeStreamingClient client;
    LlmResponse toolResponse;
    LlmToolCall call;
    call.id = QStringLiteral("tool-call-delayed-memory");
    call.name = QStringLiteral("echo_value");
    call.type = QStringLiteral("function");
    call.arguments = {{QStringLiteral("value"), QStringLiteral("ok")}};
    toolResponse.toolCalls = {call};
    FakeStreamingClient::Attempt continuation;
    continuation.deferred = true;
    continuation.success = true;
    continuation.response = textResponse(QStringLiteral("done"));
    client.attempts = {{{}, true, toolResponse, {}, false}, continuation};
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto bridge = makeRuntimeBridge();
    AgentRuntimeServices services;
    QVERIFY(AgentBootstrap::start(
        services, runtimeRequestFor(directory, &brain, bridge.get())).isOk());
    ToolRegistry tools;
    tools.registerTool(std::make_unique<EchoTool>());
    brain.setToolRegistry(&tools);
    brain.setChatSideEffectDelayForTests(150);
    int timerTicks = 0;
    QTimer frameTimer;
    frameTimer.setInterval(16);
    connect(&frameTimer, &QTimer::timeout, this, [&timerTicks]() { ++timerTicks; });
    frameTimer.start();

    brain.triggerThink(QStringLiteral("use delayed memory tool"),
                       QStringLiteral("user_request"),
                       QStringLiteral("tool-memory-dispatch"));

    QTRY_VERIFY_WITH_TIMEOUT(client.pending.has_value(), 2000);
    QTest::qWait(25);
    frameTimer.stop();
    QCOMPARE(client.routeIds.size(), 2);
    QVERIFY(timerTicks >= 1);
    QVERIFY(!persistedMemoryContains(brain.memoryStore()->databasePath(),
                                     QStringLiteral("tool_execution")));
    client.finishPendingEvenIfCancelled();
}

void StreamingDialogueTests::
finishActiveResponse_whenEventsAreQueued_shouldReflectOnlyAfterBarrier() {
    FakeStreamingClient client;
    client.attempts = {{{delta(QStringLiteral("done"))}, true,
                        textResponse(QStringLiteral("done")), {}, false}};
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto bridge = makeRuntimeBridge();
    AgentRuntimeServices services;
    QVERIFY(AgentBootstrap::start(
        services, runtimeRequestFor(directory, &brain, bridge.get())).isOk());
    std::atomic_int reflections{0};
    services.setReflectionProbeForTests(
        [&reflections](const QString&) { ++reflections; });
    brain.setChatSideEffectDelayForTests(60);
    QSignalSpy finished(&brain, &AIBrain::assistantResponseFinished);

    brain.triggerThink(QStringLiteral("需要完整事件后反思"),
                       QStringLiteral("user_request"),
                       QStringLiteral("barrier-reflection"));

    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 2000);
    QCOMPARE(reflections.load(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(reflections.load(), 1, 2000);
}

void StreamingDialogueTests::
enqueueBarrier_whenGenerationIsStale_shouldNotMutateActiveResponse() {
    FakeStreamingClient client;
    FakeStreamingClient::Attempt first;
    first.success = true;
    first.events = {delta(QStringLiteral("first"))};
    first.response = textResponse(QStringLiteral("first"));
    FakeStreamingClient::Attempt second;
    second.deferred = true;
    second.success = true;
    second.response = textResponse(QStringLiteral("second"));
    client.attempts = {first, second};
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto bridge = makeRuntimeBridge();
    AgentRuntimeServices services;
    QVERIFY(AgentBootstrap::start(
        services, runtimeRequestFor(directory, &brain, bridge.get())).isOk());
    std::atomic_int reflections{0};
    services.setReflectionProbeForTests(
        [&reflections](const QString&) { ++reflections; });
    brain.setChatSideEffectDelayForTests(60);
    QSignalSpy finished(&brain, &AIBrain::assistantResponseFinished);

    brain.triggerThink(QStringLiteral("first complex request"),
                       QStringLiteral("user_request"), QStringLiteral("first"));
    QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 2000);
    brain.triggerThink(QStringLiteral("second complex request"),
                       QStringLiteral("user_request"), QStringLiteral("second"));
    QTRY_VERIFY_WITH_TIMEOUT(client.pending.has_value(), 2000);
    QTest::qWait(450);

    QCOMPARE(finished.size(), 1);
    QVERIFY(brain.isBusy());
    QCOMPARE(reflections.load(), 0);
    brain.stopCurrentResponse();
}

void StreamingDialogueTests::
finishActiveResponse_whenToolRoundsComplete_shouldQueueEachResponseLogExactlyOnce() {
    FakeStreamingClient client;
    LlmResponse toolResponse;
    LlmToolCall call;
    call.id = QStringLiteral("tool-call-1");
    call.name = QStringLiteral("echo_value");
    call.type = QStringLiteral("function");
    call.arguments = {{QStringLiteral("value"), QStringLiteral("ok")}};
    toolResponse.toolCalls = {call};
    client.attempts = {
        {{}, true, toolResponse, {}, false},
        {{delta(QStringLiteral("finished"))}, true,
         textResponse(QStringLiteral("finished")), {}, false}
    };
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto bridge = makeRuntimeBridge();
    AgentRuntimeServices services;
    QVERIFY(AgentBootstrap::start(
        services, runtimeRequestFor(directory, &brain, bridge.get())).isOk());
    ToolRegistry tools;
    tools.registerTool(std::make_unique<EchoTool>());
    brain.setToolRegistry(&tools);
    std::atomic_int responseLogs{0};
    brain.setChatSideEffectLifecycleProbeForTests(
        [&responseLogs](const QString& phase, const QString&, quintptr) {
            if (phase == QLatin1String("response.log.completed")) ++responseLogs;
        });
    std::atomic_int reflections{0};
    services.setReflectionProbeForTests(
        [&reflections](const QString&) { ++reflections; });

    brain.triggerThink(QStringLiteral("use the echo tool"),
                       QStringLiteral("user_request"),
                       QStringLiteral("response-log-rounds"));

    QTRY_COMPARE_WITH_TIMEOUT(reflections.load(), 1, 2000);
    QCOMPARE(client.routeIds.size(), 2);
    QCOMPARE(responseLogs.load(), 2);
}

void StreamingDialogueTests::
stop_whenSideEffectsArePending_shouldNotDeliverCallbacksToDestroyedState() {
    FakeStreamingClient client;
    FakeStreamingClient::Attempt attempt;
    attempt.deferred = true;
    attempt.success = true;
    attempt.response = textResponse(QStringLiteral("late"));
    client.attempts = {attempt};
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto bridge = makeRuntimeBridge();
    AgentRuntimeServices services;
    std::atomic_int reflections{0};
    services.setReflectionProbeForTests(
        [&reflections](const QString&) { ++reflections; });
    {
        auto brain = std::make_unique<AIBrain>(
            &client, QList<ModelRoleConfig>{dialogueRoutes(
                {route(QStringLiteral("primary"))})});
        QVERIFY(AgentBootstrap::start(
            services, runtimeRequestFor(directory, brain.get(), bridge.get())).isOk());
        brain->setChatSideEffectDelayForTests(150);
        brain->triggerThink(QStringLiteral("request with pending side effects"),
                            QStringLiteral("user_request"),
                            QStringLiteral("stop-pending-effects"));
        QTRY_VERIFY_WITH_TIMEOUT(client.pending.has_value(), 2000);
        QElapsedTimer elapsed;
        elapsed.start();
        brain->stop();
        QVERIFY2(elapsed.elapsed() < 50,
                 qPrintable(QString::number(elapsed.elapsed())));
        services.stop();
    }
    QTest::qWait(400);
    QCOMPARE(reflections.load(), 0);
}

void StreamingDialogueTests::
messageSend_whenPreparationTakesOneHundredMilliseconds_shouldAllowSixteenMillisecondTimerToAdvance() {
    FakeStreamingClient client;
    client.attempts = {{{delta(QStringLiteral("done"))}, true,
                        textResponse(QStringLiteral("done")), {}, false}};
    AIBrain brain(&client, {dialogueRoutes({route(QStringLiteral("primary"))})});
    QTemporaryDir directory;
    QVERIFY(initializeBrain(brain, directory));
    brain.setChatPreparationDelayForTests(100);
    int timerTicks = 0;
    QTimer frameTimer;
    frameTimer.setInterval(16);
    connect(&frameTimer, &QTimer::timeout, this, [&timerTicks]() { ++timerTicks; });
    QSignalSpy timings(&brain, &AIBrain::chatPreparationTimingsObserved);
    frameTimer.start();

    brain.triggerThink(QStringLiteral("需要模型处理的性能回归请求"),
                       QStringLiteral("user_request"),
                       QStringLiteral("ui-frame-budget"));

    QTRY_COMPARE_WITH_TIMEOUT(client.routeIds.size(), 1, 2000);
    frameTimer.stop();
    QVERIFY(timerTicks >= 3);
    QCOMPARE(timings.size(), 1);
    const ChatPreparationTimings observed =
        qvariant_cast<ChatPreparationTimings>(timings.first().first());
    QVERIFY(observed.uiAcknowledgeMs < 16);
    QVERIFY(observed.preparationMs >= 80);
    QVERIFY(observed.dispatchLagMs >= 0);
}

QTEST_MAIN(StreamingDialogueTests)
#include "test_streaming_dialogue.moc"
