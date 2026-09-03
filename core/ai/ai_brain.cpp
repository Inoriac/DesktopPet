//
// AIBrain implementation
//

#include "ai_brain.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <utility>

#include "configLoader/config_manager.h"
#include "chat/chat_preparation_executor.h"
#include "runtime/agent_runtime_services.h"
#include "event/event_ledger.h"
#include "tools/environment_tools.h"

namespace {

qint64 monotonicMilliseconds() {
    QElapsedTimer timer;
    timer.start();
    return timer.msecsSinceReference();
}

} // namespace

AIBrain::AIBrain(QObject* parent)
    : AIBrain(nullptr, configuredModelRoles(), parent) {}

AIBrain::AIBrain(ModelCompletionClient* modelClient,
                 QList<ModelRoleConfig> modelRoles,
                 QObject* parent)
    : QObject(parent)
    , m_modelRoleRegistry(std::move(modelRoles))
    , m_modelRouter(&m_modelRoleRegistry,
                    modelClient ? modelClient : &m_modelClient) {
    m_identityBaseline = ConfigManager::instance().getIdentityBaseline();
    m_personalityPolicy = ConfigManager::instance().getPersonalityPolicy();
    m_contextBuilder.setIdentityBaseline(m_identityBaseline);
    m_chatPreparationExecutor = std::make_unique<ChatPreparationExecutor>();
    connect(m_chatPreparationExecutor.get(), &ChatPreparationExecutor::prepared,
            this, &AIBrain::continuePreparedThink);
    m_chatSideEffectQueue = std::make_unique<ChatSideEffectQueue>();
    connect(m_chatSideEffectQueue.get(), &ChatSideEffectQueue::barrierCommitted,
            this, &AIBrain::handleSideEffectBarrier);
    connect(m_chatSideEffectQueue.get(), &ChatSideEffectQueue::persistenceWarning,
            this, [](const QString& message) {
                qWarning() << "[AIBrain]" << message;
            });
    m_daydreamConfig = ConfigManager::instance().getDaydreamConfig();
    m_daydreamPolicy.configure(m_daydreamConfig);
    setupTriggerTimers();
    m_skillStore.load();
}

AIBrain::~AIBrain() {
    if (m_chatSideEffectQueue) m_chatSideEffectQueue->stop(false);
    if (m_chatPreparationExecutor) m_chatPreparationExecutor->stop();
}

Result<void, DomainError> AIBrain::initializeStorage(
    const AIBrainStorageConfig& config) {
    if (m_storageInitializationAttempted || m_running) {
        return Result<void, DomainError>::failure(
            domainError(QStringLiteral("STATE_VERSION_CONFLICT"),
                        QStringLiteral("AI brain storage initialization was already attempted")));
    }
    m_storageInitializationAttempted = true;
    if (config.databasePath.trimmed().isEmpty() || config.jsonPath.trimmed().isEmpty()) {
        return Result<void, DomainError>::failure(
            domainError(QStringLiteral("RUNTIME_START_INVALID"),
                        QStringLiteral("AI brain storage paths are empty")));
    }
    m_memoryStore.setDatabasePath(config.databasePath);
    m_memoryStore.setStoragePath(config.jsonPath);
    QString errorMessage;
    if (!m_memoryStore.load(&errorMessage)) {
        return Result<void, DomainError>::failure(
            domainError(QStringLiteral("MEMORY_STORE_UNAVAILABLE"), errorMessage));
    }
    m_storageInitialized = true;
    m_chatPreparationEnvironment = std::make_unique<ChatPreparationEnvironment>();
    m_chatPreparationEnvironment->memoryDatabasePath = m_memoryStore.databasePath();
    m_chatPreparationEnvironment->identityBaseline = m_identityBaseline;
    m_chatPreparationEnvironment->personalityPolicy = m_personalityPolicy;
    m_chatPreparationEnvironment->promptTemplate = m_promptTemplate;
    const auto preparationStarted = startChatPreparationExecutor();
    if (!preparationStarted.isOk()) {
        qWarning() << "[AIBrain] chat preparation executor unavailable:"
                   << preparationStarted.error().message;
    }
    if (!m_chatPreparationRuntimeMetadata.runtimeDatabasePath.isEmpty()) {
        const auto effectsStarted = startChatSideEffectQueue();
        if (!effectsStarted.isOk()) {
            qWarning() << "[AIBrain] chat side-effect queue unavailable:"
                       << effectsStarted.error().message;
        }
    }
    return Result<void, DomainError>::success();
}

Result<void, DomainError> AIBrain::startChatPreparationExecutor() {
    if (!m_chatPreparationExecutor || !m_chatPreparationEnvironment) {
        return Result<void, DomainError>::failure(domainError(
            QStringLiteral("CHAT_PREPARATION_UNAVAILABLE"),
            QStringLiteral("chat preparation environment is unavailable")));
    }
    return m_chatPreparationExecutor->start(*m_chatPreparationEnvironment);
}

Result<void, DomainError> AIBrain::startChatSideEffectQueue() {
    if (!m_chatSideEffectQueue || !m_storageInitialized
        || m_chatPreparationRuntimeMetadata.profileId.trimmed().isEmpty()
        || m_chatPreparationRuntimeMetadata.runtimeDatabasePath.trimmed().isEmpty()) {
        return Result<void, DomainError>::failure(domainError(
            QStringLiteral("CHAT_SIDE_EFFECT_UNAVAILABLE"),
            QStringLiteral("chat persistence environment is unavailable")));
    }
    ChatSideEffectEnvironment environment;
    environment.profileId = m_chatPreparationRuntimeMetadata.profileId;
    environment.runtimeDatabasePath =
        m_chatPreparationRuntimeMetadata.runtimeDatabasePath;
    environment.memoryDatabasePath = m_memoryStore.databasePath();
    environment.aiCallLogPath = m_callLogger.logFilePath();
    return m_chatSideEffectQueue->start(environment);
}

void AIBrain::setIdentityBaseline(const IdentityBaseline& baseline) {
    m_identityBaseline = baseline;
    m_contextBuilder.setIdentityBaseline(baseline);
    if (m_chatPreparationEnvironment) {
        m_chatPreparationEnvironment->identityBaseline = baseline;
    }
}

void AIBrain::setPromptTemplate(const PromptTemplate& templ) {
    m_promptTemplate = templ;
    m_contextBuilder.setPromptTemplate(templ);
    if (m_chatPreparationEnvironment) {
        m_chatPreparationEnvironment->promptTemplate = templ;
    }
}

void AIBrain::setRuntimeServices(AgentRuntimeServices* services) {
    if (m_runtimeServices == services) return;
    stopCurrentResponse();
    ++m_requestGeneration;
    if (m_chatSideEffectQueue) m_chatSideEffectQueue->stop(true);
    m_toolRuntime.cancelPendingConfirmations(QStringLiteral("runtime services changed"));
    m_pendingToolConfirmations.clear();
    m_runtimeSessions.clear();
    m_busy = false;
    m_runtimeServices = services;
    m_chatPreparationRuntimeMetadata = services
        ? services->chatPreparationRuntimeMetadata()
        : ChatPreparationRuntimeMetadata{};
    if (services && m_storageInitialized) {
        const auto started = startChatSideEffectQueue();
        if (!started.isOk()) {
            qWarning() << "[AIBrain] chat side-effect queue unavailable:"
                       << started.error().message;
        }
    }
}

#ifdef DESKTOP_PET_ENABLE_TEST_SEAMS
void AIBrain::setChatPreparationDelayForTests(int delayMs) {
    if (m_chatPreparationExecutor) {
        m_chatPreparationExecutor->setTestPreparationDelayMs(delayMs);
    }
}

void AIBrain::setChatSideEffectDelayForTests(int delayMs) {
    if (m_chatSideEffectQueue) {
        m_chatSideEffectQueue->setTestEffectDelayMs(delayMs);
    }
}

void AIBrain::setChatSideEffectLifecycleProbeForTests(
    ChatSideEffectQueue::LifecycleProbe probe) {
    if (m_chatSideEffectQueue) {
        m_chatSideEffectQueue->setTestLifecycleProbe(std::move(probe));
    }
}
#endif

void AIBrain::resolveToolConfirmation(const QString& requestId, bool approved) {
    const auto it = m_pendingToolConfirmations.find(requestId);
    if (it == m_pendingToolConfirmations.end()) {
        return;
    }
    std::function<void(bool)> continuation = std::move(it.value());
    m_pendingToolConfirmations.erase(it);
    continuation(approved);
}

void AIBrain::setPetName(const QString& petName) {
    m_petName = petName;
}

void AIBrain::setToolRegistry(ToolRegistry* registry) {
    if (m_toolRegistry != registry) {
        stopCurrentResponse();
        ++m_requestGeneration;
        m_pendingToolConfirmations.clear();
        m_runtimeSessions.clear();
        m_busy = false;
    }
    m_toolRegistry = registry;
    m_toolRuntime.setToolRegistry(registry);
}

void AIBrain::setAgentScheduler(AgentScheduler* scheduler) {
    m_scheduler = scheduler;
}

void AIBrain::setEmotionSnapshotProvider(EmotionSnapshotProvider provider) {
    m_emotionSnapshotProvider = std::move(provider);
}

void AIBrain::setEnabled(bool enabled) {
    m_enabled = enabled;
    if (!m_enabled) {
        stop();
    }
}

void AIBrain::setThinkIntervalMs(int ms) {
    if (ms < 1000) {
        ms = 1000;
    }
    m_idleTriggerTimer.setInterval(ms);
}

int AIBrain::userIdleSeconds() const {
    return queryUserIdleSeconds();
}

void AIBrain::setExternalSleepCoordinatorEnabled(bool enabled) {
    if (m_externalSleepCoordinatorEnabled == enabled) return;
    m_externalSleepCoordinatorEnabled = enabled;
    if (enabled) {
        if (m_daydreamRunning) {
            cancelDaydreamSession(QStringLiteral("external sleep coordinator took ownership"));
        }
        m_daydreamTimer.stop();
    } else if (m_running && m_daydreamConfig.enabled) {
        armDaydreamTimer();
    }
}

void AIBrain::start() {
    if (!m_enabled || !m_storageInitialized || m_running) {
        std::cerr << "[AIBrain] start skipped: enabled="
                  << (m_enabled ? "true" : "false")
                  << " storage=" << (m_storageInitialized ? "ready" : "unavailable")
                  << " running=" << (m_running ? "true" : "false")
                  << std::endl;
        return;
    }

    m_running = true;
    const auto preparationStarted = startChatPreparationExecutor();
    if (!preparationStarted.isOk()) {
        qWarning() << "[AIBrain] unable to restart chat preparation executor:"
                   << preparationStarted.error().message;
    }
    const auto effectsStarted = startChatSideEffectQueue();
    if (!effectsStarted.isOk()) {
        qWarning() << "[AIBrain] unable to restart chat side-effect queue:"
                   << effectsStarted.error().message;
    }
    std::cerr << "[AIBrain] runtime loop started" << std::endl;
    scheduleTrigger("idle_action");
    scheduleTrigger("proactive_chat");
    if (m_daydreamConfig.enabled && !m_externalSleepCoordinatorEnabled) {
        armDaydreamTimer();
    }
}

void AIBrain::stop() {
    if (m_daydreamRunning) {
        cancelDaydreamSession(QStringLiteral("AI brain stopped"));
    }
    stopCurrentResponse();
    ++m_requestGeneration;
    m_running = false;
    m_idleTriggerTimer.stop();
    m_chatTriggerTimer.stop();
    m_daydreamTimer.stop();
    m_idleRetryScheduled = false;
    m_toolRuntime.cancelPendingConfirmations(QStringLiteral("AI brain stopped"));
    m_pendingToolConfirmations.clear();
    m_busy = false;
    if (m_chatPreparationExecutor) m_chatPreparationExecutor->stop();
    if (m_chatSideEffectQueue) m_chatSideEffectQueue->stop(true);
    m_pendingSideEffectBarriers.clear();
    m_runtimeSessions.clear();
}

void AIBrain::triggerThink(const QString& reason,
                           const QString& triggerTag,
                           const QString& replyToId) {
    const qint64 triggerStartedAt = monotonicMilliseconds();
    const bool userInitiated = triggerTag == QLatin1String("manual")
        || triggerTag == QLatin1String("user_request")
        || triggerTag == QLatin1String("touch_event");
    if (m_runtimeServices && userInitiated) {
        m_runtimeServices->cancelSleepForUserInteraction();
    }
    if (m_daydreamRunning) {
        if (userInitiated) {
            cancelDaydreamSession(QStringLiteral("user interaction"));
        } else {
            if (m_running) scheduleTrigger(triggerTag);
            return;
        }
    }

    auto rejectUserRequest = [this, userInitiated, &replyToId, &triggerTag](
                                 const QString& errorMessage) {
        qWarning() << "[AIBrain] request rejected:"
                   << errorMessage << "trigger:" << triggerTag;
        std::cerr << "[AIBrain] request rejected: "
                  << errorMessage.toStdString()
                  << " trigger=" << triggerTag.toStdString() << std::endl;
        if (userInitiated) {
            emit thinkRequestRejected(replyToId, errorMessage);
        }
    };
    if (!m_storageInitialized) {
        rejectUserRequest(QStringLiteral("AI 运行时尚未完成初始化，请稍后重试。"));
        return;
    }
    if (!m_enabled) {
        rejectUserRequest(QStringLiteral("AI 当前没有启用，暂时不能回复。"));
        return;
    }
    if (m_busy) {
        rejectUserRequest(QStringLiteral("上一条消息仍在处理中，请稍后再试。"));
        return;
    }

    m_busy = true;
    ++m_requestGeneration;
    qInfo() << "[AIBrain] accepted request, trigger:" << triggerTag;
    std::cerr << "[AIBrain] accepted request: trigger="
              << triggerTag.toStdString() << std::endl;
    emit thinkingStarted(reason);
    beginActiveResponse(replyToId, triggerTag, {});
    const QString preparationRequestId = QUuid::createUuid().toString(
        QUuid::WithoutBraces);
    if (m_activeDialogueResponse) {
        m_activeDialogueResponse->preparationRequestId = preparationRequestId;
        m_activeDialogueResponse->uiAcknowledgeMs = qMax<qint64>(
            0, monotonicMilliseconds() - triggerStartedAt);
    }

    QString sessionId = beginPreparationRuntimeSession(reason, triggerTag);
    if (m_runtimeServices && sessionId.isEmpty()) {
        qWarning() << "[AIBrain] runtime session unavailable; continuing basic chat";
    }
    if (m_activeDialogueResponse) {
        m_activeDialogueResponse->sessionId = sessionId;
    }
    if (shouldUseLocalRouter(triggerTag)) {
        const IntentRoute route = m_intentRouter.route(reason, triggerTag);
        if (route.type != IntentRouteType::NeedLLM) {
            bindLocalRuntimeSnapshot(sessionId);
            if (m_runtimeServices && !sessionId.isEmpty() && !appendRuntimeEvent(
                    QStringLiteral("UserMessageReceived"), sessionId,
                    {{QStringLiteral("text"), reason},
                     {QStringLiteral("triggerTag"), triggerTag}})) {
                qWarning() << "[AIBrain] unable to record local user message event";
            }
            enqueueUserMemoryWrite(reason, triggerTag, preparationRequestId,
                                   m_requestGeneration, sessionId);
            if (tryHandleRoutedIntent(route, reason, triggerTag, sessionId)) return;
        }
    }

    if (m_runtimeServices && !sessionId.isEmpty() && !appendRuntimeEvent(
            QStringLiteral("UserMessageReceived"), sessionId,
            {{QStringLiteral("text"), reason},
             {QStringLiteral("triggerTag"), triggerTag}})) {
        qWarning() << "[AIBrain] unable to queue user message event";
    }

    if (m_activeDialogueResponse) {
        m_activeDialogueResponse->reason = reason;
    }
    if (!m_chatPreparationExecutor) {
        finishActiveResponse(ChatMessageStatus::Failed,
                             QStringLiteral("Chat preparation is unavailable"));
        return;
    }
    m_chatPreparationExecutor->submit(makeChatPreparationRequest(
        preparationRequestId, m_requestGeneration, reason, triggerTag, sessionId));
}

QString AIBrain::beginPreparationRuntimeSession(const QString& reason,
                                                const QString& triggerTag) {
    if (!m_runtimeServices) return QString();
    AgentSession session = AgentSession::create(reason, triggerTag);
    const QString sessionId = session.id();
    m_runtimeSessions.insert(sessionId, std::move(session));
    return sessionId;
}

bool AIBrain::bindLocalRuntimeSnapshot(const QString& sessionId) {
    if (!m_runtimeServices || sessionId.isEmpty()) return true;
    auto session = m_runtimeSessions.find(sessionId);
    if (session == m_runtimeSessions.end()) return false;
    // Local routes never enqueue model preparation. Capture their identity
    // versions only after routing has proved the request will stay local.
    const RuntimeSnapshot snapshot = m_runtimeServices->captureSnapshot(
        sessionId, QStringLiteral("owner"));
    return !snapshot.sessionId.isEmpty()
        && session->bindRuntimeSnapshot(snapshot).isOk();
}

void AIBrain::continuePreparedThink(ChatPreparationResult result) {
    if (!m_activeDialogueResponse || m_activeDialogueResponse->terminal
        || result.generation != m_requestGeneration
        || result.generation != m_activeDialogueResponse->generation
        || result.requestId != m_activeDialogueResponse->preparationRequestId) {
        return;
    }
    if (!result.error.code.isEmpty() || result.messages.isEmpty()) {
        const QString error = result.error.message.isEmpty()
            ? QStringLiteral("Unable to build model context") : result.error.message;
        finishActiveResponse(ChatMessageStatus::Failed, error);
        return;
    }

    const QString reason = m_activeDialogueResponse->reason;
    const QString triggerTag = m_activeDialogueResponse->triggerTag;
    const QString sessionId = m_activeDialogueResponse->sessionId;
    m_activeDialogueResponse->reinforcementIds = result.reinforcementIds;
    m_activeDialogueResponse->preparedAtMonotonicMs = monotonicMilliseconds();
    m_activeDialogueResponse->preparationMs = qMax<qint64>(
        0, result.preparationDurationMs);

    if (m_runtimeServices && !sessionId.isEmpty()) {
        auto session = m_runtimeSessions.find(sessionId);
        if (session == m_runtimeSessions.end() || !result.runtimeSnapshot.has_value()
            || !session->bindRuntimeSnapshot(*result.runtimeSnapshot).isOk()) {
            finishActiveResponse(ChatMessageStatus::Failed,
                                 QStringLiteral("Unable to bind runtime snapshot"));
            return;
        }
    }

    const MemoryReinforcementBatch reinforcement =
        m_memoryStore.stageReinforcement(result.reinforcementIds);
    if (!reinforcement.entries.isEmpty() && m_chatSideEffectQueue
        && m_chatSideEffectQueue->isAccepting()) {
        DeferredChatSideEffect effect;
        effect.type = ChatSideEffectType::MemoryReinforcement;
        effect.requestId = result.requestId;
        effect.generation = result.generation;
        effect.sessionId = sessionId;
        effect.reinforcedEntries = reinforcement.entries;
        if (!m_chatSideEffectQueue->tryEnqueue(std::move(effect))) {
            m_memoryStore.rollbackReinforcement(reinforcement);
            qWarning() << "[AIBrain] unable to queue memory reinforcement";
        }
    } else if (!reinforcement.entries.isEmpty()) {
        m_memoryStore.rollbackReinforcement(reinforcement);
        qWarning() << "[AIBrain] memory reinforcement queue is unavailable";
    }
    enqueueUserMemoryWrite(reason, triggerTag, result.requestId,
                           result.generation, sessionId);
    thinkInternal(reason, triggerTag, sessionId, 0, result.messages);
}

void AIBrain::beginActiveResponse(const QString& replyToId,
                                  const QString& triggerTag,
                                  const QString& sessionId) {
    ActiveDialogueResponse response;
    response.messageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    response.replyToId = replyToId;
    response.triggerTag = triggerTag;
    response.sessionId = sessionId;
    response.generation = m_requestGeneration;
    response.acceptedAtMonotonicMs = monotonicMilliseconds();
    m_activeDialogueResponse.emplace(std::move(response));
    emit assistantResponseStarted(m_activeDialogueResponse->messageId,
                                  m_activeDialogueResponse->replyToId,
                                  m_activeDialogueResponse->triggerTag);
    emit assistantResponseStageChanged(m_activeDialogueResponse->messageId,
                                       ChatActivityStage::WaitingForModel);
}

void AIBrain::publishActiveStage(ChatActivityStage stage) {
    if (!m_activeDialogueResponse || m_activeDialogueResponse->terminal
        || m_activeDialogueResponse->stage == stage) {
        return;
    }
    m_activeDialogueResponse->stage = stage;
    emit assistantResponseStageChanged(m_activeDialogueResponse->messageId, stage);
}

void AIBrain::appendActiveDelta(const QString& textDelta) {
    if (!m_activeDialogueResponse || m_activeDialogueResponse->terminal
        || textDelta.isEmpty()) {
        return;
    }
    m_activeDialogueResponse->visibleContent += textDelta;
    m_activeDialogueResponse->status = ChatMessageStatus::Streaming;
    publishActiveStage(ChatActivityStage::StreamingText);
    emit assistantResponseDelta(m_activeDialogueResponse->messageId, textDelta);
}

void AIBrain::finishActiveResponse(ChatMessageStatus status,
                                   const QString& errorMessage,
                                   const QJsonObject& responseLog) {
    if (!m_activeDialogueResponse || m_activeDialogueResponse->terminal) return;
    m_activeDialogueResponse->terminal = true;
    m_activeDialogueResponse->status = status;
    ActiveDialogueResponse finished = std::move(*m_activeDialogueResponse);
    finished.requestHandle.reset();
    m_activeDialogueResponse.reset();

    if (status == ChatMessageStatus::Complete
        && !finished.visibleContent.isEmpty()) {
        appendRuntimeEvent(
            QStringLiteral("AssistantResponseProduced"), finished.sessionId,
            {{QStringLiteral("text"), finished.visibleContent},
             {QStringLiteral("triggerTag"), finished.triggerTag},
             {QStringLiteral("modelRole"), QStringLiteral("dialogue")}},
            finished.preparationRequestId, finished.generation);
        ChatMessage assistantMessage;
        assistantMessage.role = QStringLiteral("assistant");
        assistantMessage.content = finished.visibleContent;
        appendToMemory(assistantMessage);
    }

    if (!responseLog.isEmpty()) {
        enqueueCallLog(ChatSideEffectType::ResponseLog,
                       responseLog.value(QStringLiteral("request_id")).toString(),
                       finished.sessionId, finished.generation, responseLog);
    }

    finishRuntimeSession(finished.sessionId, finished.generation);
    m_busy = false;
    if (status == ChatMessageStatus::Complete
        && !finished.visibleContent.isEmpty()) {
        emit assistantResponseReady(finished.visibleContent);
        if (finished.triggerTag == QLatin1String("proactive_chat")) {
            emit proactiveResponseReady(finished.visibleContent);
        }
    }
    emit thinkingFinished(status == ChatMessageStatus::Complete, errorMessage);
    emit assistantResponseFinished(finished.messageId, status, errorMessage);
}

void AIBrain::stopCurrentResponse() {
    if (!m_activeDialogueResponse || m_activeDialogueResponse->terminal) return;
    const QString sessionId = m_activeDialogueResponse->sessionId;
    const quint64 generation = m_activeDialogueResponse->generation;
    const auto pendingIds = m_pendingResponseLogs.keys();
    for (const QString& requestId : pendingIds) {
        const PendingResponseLog pending = m_pendingResponseLogs.value(requestId);
        if (pending.sessionId != sessionId || pending.generation != generation
            || !pending.handled
            || pending.handled->exchange(true, std::memory_order_acq_rel)) {
            continue;
        }
        enqueueCallLog(
            ChatSideEffectType::ResponseLog, requestId, sessionId, generation,
            AiCallLogger::responseRecord(
                requestId, m_petName, false, {},
                QStringLiteral("LLM_REQUEST_CANCELLED")));
        m_pendingResponseLogs.remove(requestId);
    }
    ++m_requestGeneration;
    if (m_activeDialogueResponse->requestHandle) {
        m_activeDialogueResponse->requestHandle->cancel();
    }
    m_toolRuntime.cancelPendingConfirmations(
        QStringLiteral("LLM_REQUEST_CANCELLED"));
    m_pendingToolConfirmations.clear();
    finishActiveResponse(ChatMessageStatus::Stopped,
                         QStringLiteral("LLM_REQUEST_CANCELLED"));
}

QString AIBrain::beginRuntimeSession(const QString& reason,
                                     const QString& triggerTag) {
    if (!m_runtimeServices) return QString();
    AgentSession session = AgentSession::create(reason, triggerTag);
    const QString sessionId = session.id();
    const RuntimeSnapshot snapshot = m_runtimeServices->captureSnapshot(
        sessionId, QStringLiteral("owner"));
    if (snapshot.sessionId.isEmpty() || !session.bindRuntimeSnapshot(snapshot).isOk()) {
        return QString();
    }
    m_runtimeSessions.insert(sessionId, std::move(session));
    return sessionId;
}

void AIBrain::finishRuntimeSession(const QString& sessionId, quint64 generation) {
    if (sessionId.isEmpty()) return;
    if (m_chatSideEffectQueue && m_chatSideEffectQueue->isAccepting()) {
        m_pendingSideEffectBarriers.insert(sessionId, generation);
        if (m_chatSideEffectQueue->tryEnqueueBarrier(sessionId, generation)) {
            return;
        }
        m_pendingSideEffectBarriers.remove(sessionId);
        qWarning() << "[AIBrain] unable to queue session persistence barrier";
    }
    m_runtimeSessions.remove(sessionId);
}

void AIBrain::handleSideEffectBarrier(const QString& sessionId,
                                      quint64 generation) {
    const auto pending = m_pendingSideEffectBarriers.constFind(sessionId);
    if (pending == m_pendingSideEffectBarriers.constEnd()
        || pending.value() != generation) {
        return;
    }
    m_pendingSideEffectBarriers.remove(sessionId);
    if (generation == m_requestGeneration && m_runtimeServices
        && m_runtimeSessions.contains(sessionId)) {
        m_runtimeServices->reflectOnCompletedSession(sessionId);
    }
    m_runtimeSessions.remove(sessionId);
}

void AIBrain::enqueueCallLog(ChatSideEffectType type,
                             const QString& requestId,
                             const QString& sessionId,
                             quint64 generation,
                             QJsonObject record) {
    if (!m_chatSideEffectQueue || !m_chatSideEffectQueue->isAccepting()) return;
    DeferredChatSideEffect effect;
    effect.type = type;
    effect.requestId = requestId;
    effect.sessionId = sessionId;
    effect.generation = generation;
    effect.logRecord = std::move(record);
    m_chatSideEffectQueue->enqueue(std::move(effect));
}

bool AIBrain::appendRuntimeEvent(const QString& type,
                                 const QString& sessionId,
                                 const QJsonObject& payload,
                                 const QString& requestId,
                                 quint64 generation) {
    if (!m_runtimeServices) return true;
    if (!m_runtimeServices->eventLedger()) return true;
    if (!m_chatSideEffectQueue || !m_chatSideEffectQueue->isAccepting()) return false;
    const auto session = m_runtimeSessions.constFind(sessionId);
    QString profileId = m_chatPreparationRuntimeMetadata.profileId;
    if (session != m_runtimeSessions.constEnd()
        && session->runtimeSnapshot().has_value()) {
        profileId = session->runtimeSnapshot()->profileId;
    }
    if (profileId.trimmed().isEmpty()) return false;
    EventDraft draft;
    draft.profileId = profileId;
    draft.type = type;
    draft.source = QStringLiteral("AIBrain");
    draft.sessionId = sessionId;
    draft.payload = payload;
    DeferredChatSideEffect effect;
    effect.type = ChatSideEffectType::RuntimeEvent;
    effect.requestId = !requestId.isEmpty()
        ? requestId
        : m_activeDialogueResponse
            ? m_activeDialogueResponse->preparationRequestId : sessionId;
    effect.generation = generation > 0
        ? generation
        : m_activeDialogueResponse
            ? m_activeDialogueResponse->generation : m_requestGeneration;
    effect.sessionId = sessionId;
    effect.event = std::move(draft);
    return m_chatSideEffectQueue->tryEnqueue(std::move(effect));
}

void AIBrain::onUserInteraction(const QString& eventName, const QString& detail) {
    const QString reason = detail.isEmpty()
                             ? QString("user_event:%1").arg(eventName)
                             : QString("user_event:%1:%2").arg(eventName, detail);
    triggerThink(reason, "touch_event");
}

void AIBrain::clearMemory() {
    m_memory.clear();
    m_memoryStore.clear();
    m_memoryStore.save();
}

bool AIBrain::shouldUseLocalRouter(const QString& triggerTag) const {
    return triggerTag == "manual" || triggerTag == "user_request";
}

void AIBrain::enqueueUserMemoryWrite(const QString& input,
                                     const QString& triggerTag,
                                     const QString& requestId,
                                     quint64 generation,
                                     const QString& sessionId) {
    if (!shouldUseLocalRouter(triggerTag)) {
        return;
    }

    QList<MemoryCandidate> candidates = m_memoryExtractor.extractFromUserInput(input, triggerTag);
    if (!candidates.isEmpty()) {
        for (MemoryCandidate& candidate : candidates) {
            if (candidate.operation == MemoryCandidateOperation::Write) {
                annotateMemoryEntry(candidate.entry);
            }
        }
    }
    MemoryEntry impression;
    if (candidates.isEmpty() && m_daydreamConfig.enabled) {
        impression = m_memoryExtractor.extractDaydreamImpression(input, triggerTag);
        if (!impression.content.isEmpty()) annotateMemoryEntry(impression);
    }
    if (candidates.isEmpty() && impression.content.isEmpty()) return;

    MemoryMutationBatch mutations;
    if (!candidates.isEmpty()) {
        mutations = m_memoryPolicy.stageCandidates(candidates, &m_memoryStore).mutations;
    } else {
        bool staged = false;
        for (const MemoryEntry& existing : m_memoryStore.all()) {
            if (existing.status != MemoryStatus::Active
                || existing.partition != QLatin1String("hippocampus")
                || existing.key != impression.key) {
                continue;
            }
            MemoryEntry updated = existing;
            updated.mentionCount = qMax(1, existing.mentionCount) + 1;
            updated.updatedAt = QDateTime::currentDateTimeUtc();
            if (!updated.evidence.contains(impression.content)) {
                updated.evidence.append(impression.content);
            }
            staged = m_memoryStore.stageEntryUpdate(updated, &mutations);
            break;
        }
        if (!staged) {
            int pendingCount = 0;
            for (const MemoryEntry& entry : m_memoryStore.all()) {
                if (entry.status == MemoryStatus::Active
                    && entry.partition == QLatin1String("hippocampus")) {
                    ++pendingCount;
                }
            }
            if (m_daydreamConfig.inboxLimit <= 0
                || pendingCount < m_daydreamConfig.inboxLimit) {
                m_memoryStore.stageEntry(impression, &mutations);
            }
        }
    }
    if (mutations.isEmpty()) return;
    if (!m_chatSideEffectQueue || !m_chatSideEffectQueue->isAccepting()) {
        m_memoryStore.rollbackMutationBatch(mutations);
        qWarning() << "[AIBrain] user memory persistence queue is unavailable";
        return;
    }

    DeferredChatSideEffect effect;
    effect.type = ChatSideEffectType::UserMemoryWrite;
    effect.requestId = requestId;
    effect.generation = generation;
    effect.sessionId = sessionId;
    effect.memoryMutations = mutations;
    if (!m_chatSideEffectQueue->tryEnqueue(std::move(effect))) {
        m_memoryStore.rollbackMutationBatch(mutations);
        qWarning() << "[AIBrain] unable to queue user memory persistence";
    }
}

std::optional<EmotionSnapshot> AIBrain::currentEmotionSnapshot() const {
    if (!m_emotionSnapshotProvider) {
        return std::nullopt;
    }
    const std::optional<EmotionSnapshot> snapshot = m_emotionSnapshotProvider();
    const int activeEmotion = snapshot.has_value()
        ? static_cast<int>(snapshot->active)
        : -1;
    if (!snapshot.has_value()
        || !snapshot->updatedAt.isValid()
        || activeEmotion < static_cast<int>(EmotionType::Neutral)
        || activeEmotion > static_cast<int>(EmotionType::Surprise)
        || !std::isfinite(snapshot->moodValence)
        || !std::isfinite(snapshot->moodArousal)
        || !std::isfinite(snapshot->intensity)
        || !std::isfinite(snapshot->confidence)
        || snapshot->moodValence < -1.0
        || snapshot->moodValence > 1.0
        || snapshot->moodArousal < 0.0
        || snapshot->moodArousal > 1.0
        || snapshot->intensity < 0.0
        || snapshot->intensity > 1.0
        || snapshot->confidence < 0.0
        || snapshot->confidence > 1.0) {
        return std::nullopt;
    }
    return snapshot;
}

void AIBrain::annotateMemoryEntry(MemoryEntry& entry) const {
    const std::optional<EmotionSnapshot> snapshot = currentEmotionSnapshot();
    if (!snapshot.has_value()
        || snapshot->active == EmotionType::Neutral
        || snapshot->intensity < 0.60
        || snapshot->confidence < 0.60) {
        return;
    }
    entry.emotion = snapshot->active;
    entry.emotionIntensity = std::clamp(snapshot->intensity, 0.0, 1.0);
    entry.emotionConfidence = std::clamp(snapshot->confidence, 0.0, 1.0);
}
