//
// AIBrain implementation
//

#include "ai_brain.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QCoreApplication>
#include <QDir>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <utility>

#include "configLoader/config_manager.h"
#include "runtime/agent_runtime_services.h"
#include "event/event_ledger.h"
#include "tools/environment_tools.h"

AIBrain::AIBrain(QObject* parent)
    : AIBrain(nullptr, configuredModelRoles(), parent) {}

AIBrain::AIBrain(ModelCompletionClient* modelClient,
                 QList<ModelRoleConfig> modelRoles,
                 QObject* parent)
    : QObject(parent)
    , m_modelRoleRegistry(std::move(modelRoles))
    , m_modelRouter(&m_modelRoleRegistry,
                    modelClient ? modelClient : &m_modelClient) {
    m_daydreamConfig = ConfigManager::instance().getDaydreamConfig();
    m_daydreamPolicy.configure(m_daydreamConfig);
    setupTriggerTimers();
    m_skillStore.load();
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
    return Result<void, DomainError>::success();
}

void AIBrain::setRuntimeServices(AgentRuntimeServices* services) {
    if (m_runtimeServices == services) return;
    stopCurrentResponse();
    ++m_requestGeneration;
    m_toolRuntime.cancelPendingConfirmations(QStringLiteral("runtime services changed"));
    m_pendingToolConfirmations.clear();
    m_runtimeSessions.clear();
    m_busy = false;
    m_runtimeServices = services;
}

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
        return;
    }

    m_running = true;
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
    m_runtimeSessions.clear();
    m_busy = false;
}

void AIBrain::triggerThink(const QString& reason,
                           const QString& triggerTag,
                           const QString& replyToId) {
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
    if (!m_storageInitialized || !m_enabled || m_busy) {
        return;
    }

    const QString sessionId = beginRuntimeSession(reason, triggerTag);
    if (m_runtimeServices && sessionId.isEmpty()) return;
    if (m_runtimeServices && !appendRuntimeEvent(
            QStringLiteral("UserMessageReceived"), sessionId,
            {{QStringLiteral("text"), reason},
             {QStringLiteral("triggerTag"), triggerTag}})) {
        finishRuntimeSession(sessionId);
        return;
    }

    processUserMemoryWrite(reason, triggerTag);

    emit thinkingStarted(reason);
    m_busy = true;
    beginActiveResponse(replyToId, triggerTag, sessionId);

    if (shouldUseLocalRouter(triggerTag)
        && tryHandleRoutedIntent(reason, triggerTag, sessionId)) {
        return;
    }

    QList<ChatMessage> base = buildBaseMessages(reason, triggerTag, sessionId);
    if (base.isEmpty()) {
        finishActiveResponse(ChatMessageStatus::Failed,
                             QStringLiteral("Unable to build model context"));
        return;
    }
    thinkInternal(reason, triggerTag, sessionId, 0, base);
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
                                   const QString& errorMessage) {
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
             {QStringLiteral("modelRole"), QStringLiteral("dialogue")}});
        ChatMessage assistantMessage;
        assistantMessage.role = QStringLiteral("assistant");
        assistantMessage.content = finished.visibleContent;
        appendToMemory(assistantMessage);
    }

    finishRuntimeSession(finished.sessionId);
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

void AIBrain::finishRuntimeSession(const QString& sessionId) {
    if (sessionId.isEmpty()) return;
    if (m_runtimeServices && m_runtimeSessions.contains(sessionId)) {
        m_runtimeServices->reflectOnCompletedSession(sessionId);
    }
    m_runtimeSessions.remove(sessionId);
}

bool AIBrain::appendRuntimeEvent(const QString& type,
                                 const QString& sessionId,
                                 const QJsonObject& payload) {
    if (!m_runtimeServices) return true;
    EventLedger* ledger = m_runtimeServices->eventLedger();
    if (!ledger) return true;
    const auto session = m_runtimeSessions.constFind(sessionId);
    if (session == m_runtimeSessions.constEnd()
        || !session->runtimeSnapshot().has_value()) {
        return false;
    }
    EventDraft draft;
    draft.profileId = session->runtimeSnapshot()->profileId;
    draft.type = type;
    draft.source = QStringLiteral("AIBrain");
    draft.sessionId = sessionId;
    draft.payload = payload;
    return ledger->append(draft).isOk();
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

void AIBrain::processUserMemoryWrite(const QString& input,
                                     const QString& triggerTag) {
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
        // Explicit remember/forget requests keep their deterministic, immediate
        // semantics and must not also be duplicated into the Daydream inbox.
        m_memoryPolicy.applyCandidates(candidates, &m_memoryStore);
        return;
    }

    if (!m_daydreamConfig.enabled) {
        return;
    }

    MemoryEntry impression = m_memoryExtractor.extractDaydreamImpression(input, triggerTag);
    if (impression.content.isEmpty()) return;
    annotateMemoryEntry(impression);

    // Coalesce exact repeated self-disclosures so recurrence becomes a useful
    // consolidation signal instead of creating duplicate inbox rows.
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
        m_memoryStore.updateEntryById(updated);
        return;
    }

    DaydreamConsolidator consolidator(m_memoryStore);
    if (consolidator.pendingCount() >= m_daydreamConfig.inboxLimit) {
        qWarning() << "[Daydream] inbox capacity reached; skipping new impression";
        return;
    }
    m_memoryStore.addEntry(impression);
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
