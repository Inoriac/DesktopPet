#include "chat_side_effect_queue.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QSqlError>
#include <QTimer>
#include <QUuid>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QThread>

#include <algorithm>
#include <utility>

#include "ai/ai_call_logger.h"
#include "ai/event/event_ledger.h"
#include "ai/event/event_schema_registry.h"
#include "ai/event/sqlite_event_repository.h"
#include "ai/memory/memory_store.h"
#include "ai/memory/memory_attribution_service.h"
#include "ai/memory/memory_policy.h"
#include "ai/memory/daydream_consolidator.h"

struct ChatSideEffectQueue::ProbeState {
    QMutex mutex;
    LifecycleProbe probe;
};

struct ChatSideEffectQueue::QueueState {
    QMutex mutex;
    QList<DeferredChatSideEffect> pending;
    QSet<QString> barriersAccepted;
    std::shared_ptr<std::atomic_int> totalDepth;
    int maxDepth = 256;
    int depth = 0;
    bool accepting = false;
    bool pumpScheduled = false;
    bool stopRequested = false;
    bool drainAcceptedWork = true;
    bool workerClosed = false;
};

class ChatSideEffectQueue::Worker final : public QObject {
public:
    Worker(ChatSideEffectEnvironment environment,
           std::shared_ptr<QueueState> state,
           std::shared_ptr<std::atomic_int> delayMs,
           std::shared_ptr<ProbeState> probeState)
        : m_environment(std::move(environment))
        , m_state(std::move(state))
        , m_delayMs(std::move(delayMs))
        , m_probeState(std::move(probeState)) {}

    Result<void, DomainError> initialize() {
        m_schemas = std::make_unique<EventSchemaRegistry>();
        const auto registered = registerBuiltInEventSchemas(*m_schemas);
        if (!registered.isOk()) return registered;
        m_schemas->freeze();

        m_eventRepository = std::make_unique<SqliteEventRepository>();
        const auto eventOpened = m_eventRepository->open(
            m_environment.runtimeDatabasePath);
        if (!eventOpened.isOk()) return eventOpened;
        report(QStringLiteral("event.open"), QString());
        m_eventLedger = std::make_unique<SqliteEventLedger>(
            m_eventRepository.get(), m_schemas.get(), m_environment.profileId);

        m_memoryStore = std::make_unique<MemoryStore>();
        m_memoryStore->setProfileId(m_environment.profileId);
        m_memoryStore->setDatabasePath(m_environment.memoryDatabasePath);
        m_memoryStore->setStoragePath(QString());
        QString memoryError;
        if (!m_memoryStore->loadDatabaseOnly(&memoryError)) {
            return Result<void, DomainError>::failure(domainError(
                QStringLiteral("MEMORY_STORE_UNAVAILABLE"), memoryError));
        }
        QTimer::singleShot(0, this, [this] { migrateNextBatch(); });
        report(QStringLiteral("memory.open"), QString());
        m_logger = std::make_unique<AiCallLogger>(m_environment.aiCallLogPath);
        return Result<void, DomainError>::success();
    }

    void pump() {
        while (true) {
            DeferredChatSideEffect effect;
            bool hasEffect = false;
            bool shouldClose = false;
            {
                QMutexLocker lock(&m_state->mutex);
                if (m_state->stopRequested && !m_state->drainAcceptedWork) {
                    discardPendingLocked();
                }
                if (m_state->pending.isEmpty()) {
                    m_state->pumpScheduled = false;
                    shouldClose = m_state->stopRequested;
                } else {
                    effect = std::move(m_state->pending.front());
                    m_state->pending.pop_front();
                    hasEffect = true;
                }
            }
            if (shouldClose) {
                shutdownAndQuit();
                return;
            }
            if (!hasEffect) {
                const QStringList changed(m_changedMemoryIds.begin(), m_changedMemoryIds.end());
                m_changedMemoryIds.clear();
                if (drainedReady) drainedReady(changed);
                return;
            }
            process(std::move(effect));
        }
    }

    void shutdownAndQuit() {
        if (m_closed) return;
        m_closed = true;
        {
            QMutexLocker lock(&m_state->mutex);
            m_state->workerClosed = true;
            m_state->accepting = false;
        }
        m_logger.reset();
        m_memoryStore.reset();
        m_eventLedger.reset();
        if (m_eventRepository) m_eventRepository->close();
        m_eventRepository.reset();
        m_schemas.reset();
        report(QStringLiteral("worker.closed"), QString());
        QThread::currentThread()->quit();
    }

    std::function<void(const QStringList&)> drainedReady;
    std::function<void(const QString&, quint64)> barrierReady;
    std::function<void(const QString&)> warningReady;

private:
    void process(DeferredChatSideEffect effect) {
        if (!waitForTestDelay()) {
            completeEffect(effect, QStringLiteral("effect.discarded"));
            return;
        }
        bool ok = true;
        QString completionPhase;
        switch (effect.type) {
        case ChatSideEffectType::RuntimeEvent:
            ok = appendEvent(effect.event);
            completionPhase = QStringLiteral("runtime.event.completed");
            break;
        case ChatSideEffectType::MemoryReinforcement:
            for (const auto& entry : effect.reinforcedEntries) m_changedMemoryIds.insert(entry.id);
            ok = persistReinforcement(
                effect.reinforcedEntries, effect.sessionId,
                effect.memoryContext.value_or(ContextSpec::localOwner(m_environment.profileId)));
            completionPhase = QStringLiteral("memory.reinforcement.completed");
            break;
        case ChatSideEffectType::UserMemoryWrite:
            for (const auto& change : effect.memoryMutations.entries) m_changedMemoryIds.insert(change.after.id);
            ok = effect.memoryCandidates.isEmpty() ? persistMemoryMutations(effect.memoryMutations)
                                                   : persistCandidates(effect);
            completionPhase = QStringLiteral("user.memory.write.completed");
            break;
        case ChatSideEffectType::MemoryTask: {
            const QJsonObject result = effect.memoryTask && m_memoryStore
                ? effect.memoryTask(*m_memoryStore) : QJsonObject{};
            for (const auto& id : result.value("changedMemoryIds").toArray()) m_changedMemoryIds.insert(id.toString());
            if (effect.memoryTaskCompleted) effect.memoryTaskCompleted(result);
            completionPhase = QStringLiteral("memory.task.completed");
            break;
        }
        case ChatSideEffectType::RequestLog:
            ok = m_logger && m_logger->appendRecord(effect.logRecord);
            completionPhase = QStringLiteral("request.log.completed");
            break;
        case ChatSideEffectType::ResponseLog:
            ok = m_logger && m_logger->appendRecord(effect.logRecord);
            completionPhase = QStringLiteral("response.log.completed");
            break;
        case ChatSideEffectType::Barrier:
            completionPhase = QStringLiteral("barrier.completed");
            break;
        }

        if (!ok && warningReady) {
            warningReady(QStringLiteral(
                "A non-critical chat persistence operation failed"));
        }
        completeEffect(effect, completionPhase);
        if (effect.type == ChatSideEffectType::Barrier && ok && barrierReady) {
            barrierReady(effect.sessionId, effect.generation);
        }
    }

    bool waitForTestDelay() const {
        int remaining = m_delayMs->load(std::memory_order_acquire);
        while (remaining > 0) {
            {
                QMutexLocker lock(&m_state->mutex);
                if (m_state->stopRequested && !m_state->drainAcceptedWork) {
                    return false;
                }
            }
            if (QThread::currentThread()->isInterruptionRequested()) return false;
            const int slice = qMin(remaining, 5);
            QThread::msleep(static_cast<unsigned long>(slice));
            remaining -= slice;
        }
        return true;
    }

    void completeEffect(const DeferredChatSideEffect& effect,
                        const QString& completionPhase) {
        report(completionPhase, effect.sessionId);
        report(QStringLiteral("effect.completed"), effect.sessionId);
        QMutexLocker lock(&m_state->mutex);
        --m_state->depth;
        m_state->totalDepth->fetch_sub(1, std::memory_order_acq_rel);
    }

    bool appendEvent(const EventDraft& original) {
        if (!m_eventLedger) return false;
        EventDraft event = original;
        if (event.eventId.isEmpty())
            event.eventId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!event.occurredAt.isValid())
            event.occurredAt = QDateTime::currentDateTimeUtc();
        auto result = m_eventLedger->appendIdempotent(event);
        if (result.isOk()) return true;
        const QString message = result.error().message.toLower();
        if (!message.contains(QStringLiteral("busy"))
            && !message.contains(QStringLiteral("locked"))) {
            return false;
        }
        QThread::msleep(5);
        return m_eventLedger->appendIdempotent(event).isOk();
    }

    bool persistReinforcement(const QList<MemoryEntry>& stagedEntries, const QString& sessionId,
                              const ContextSpec& context) {
        if (persistReinforcementOnce(stagedEntries, sessionId, context))
            return true;
        QThread::msleep(5);
        return m_memoryStore && m_memoryStore->refreshDatabaseOnly() &&
               persistReinforcementOnce(stagedEntries, sessionId, context);
    }

    bool persistReinforcementOnce(const QList<MemoryEntry>& stagedEntries, const QString& sessionId,
                                  const ContextSpec& context) {
        if (!m_memoryStore || stagedEntries.isEmpty()) return true;
        // Keep the authoritative status read and its reinforcement write in
        // one SQLite snapshot. A scheduler completion committed meanwhile must
        // make this attempt fail and retry, rather than restore the old Active row.
        if (!m_memoryStore->beginTransaction()) return false;
        if (!m_memoryStore->refreshDatabaseOnly()) {
            m_memoryStore->rollbackTransaction();
            return false;
        }

        QList<MemoryEntry> updates;
        QSet<QString> seen;
        for (const MemoryEntry& staged : stagedEntries) {
            if (staged.id.trimmed().isEmpty() || seen.contains(staged.id)) continue;
            seen.insert(staged.id);
            const MemoryEntry* persisted = m_memoryStore->findById(staged.id);
            if (!persisted || persisted->status != MemoryStatus::Active ||
                persisted->privacyLevel == PrivacyLevel::Sensitive ||
                (persisted->expiresAt.isValid() && persisted->expiresAt <= QDateTime::currentDateTimeUtc()) ||
                !memoryVisibleInContext(persisted->attribution, context))
                continue;
            MemoryEntry updated = *persisted;
            updated.strength = qMin(1.0, updated.strength + 0.1);
            updated.accessCount += 1;
            updated.lastAccessedAt = staged.lastAccessedAt.isValid()
                ? staged.lastAccessedAt.toUTC() : QDateTime::currentDateTimeUtc();
            updated.updatedAt = updated.lastAccessedAt;
            updates.append(std::move(updated));
        }
        report(QStringLiteral("memory.reinforcement.read"), sessionId);
        for (const MemoryEntry& update : updates) {
            if (!m_memoryStore->updateEntryById(update)) {
                m_memoryStore->rollbackTransaction();
                return false;
            }
        }
        if (!m_memoryStore->commitTransaction()) {
            m_memoryStore->rollbackTransaction();
            return false;
        }
        report(QStringLiteral("memory.transaction.commit"), sessionId);
        return true;
    }

    QSet<QString> m_changedMemoryIds;

    bool persistMemoryMutations(const MemoryMutationBatch& mutations) {
        if (!m_memoryStore || mutations.isEmpty()) return true;
        QStringList written;
        bool ok = m_memoryStore->persistMutationBatch(mutations, &written);
        if (!ok) {
            QThread::msleep(5);
            ok = m_memoryStore->refreshDatabaseOnly()
                && m_memoryStore->persistMutationBatch(mutations, &written);
        }
        for (const auto& id : written) m_changedMemoryIds.insert(id);
        return ok;
    }

    MemoryAttributionService attributionService() {
        return MemoryAttributionService(
            m_environment.profileId, m_memoryStore.get(),
            [this](const QString& profile,
                   const QString& id) -> Result<std::optional<MemoryOriginSource>, DomainError> {
                using R = Result<std::optional<MemoryOriginSource>, DomainError>;
                if (!m_eventRepository || !m_eventRepository->isOpen())
                    return R::failure(
                        domainError("STORE_UNAVAILABLE", "Runtime source store unavailable"));
                QSqlQuery q(QSqlDatabase::database(m_eventRepository->connectionName()));
                q.prepare("SELECT profile_id,type,source,payload_json,occurred_at FROM event_log "
                          "WHERE event_id=?");
                q.addBindValue(id);
                if (!q.exec())
                    return R::failure(domainError("STORE_UNAVAILABLE", q.lastError().text()));
                if (!q.next())
                    return R::success(std::nullopt);
                MemoryOriginSource origin;
                origin.originId = id;
                origin.profileId = q.value(0).toString();
                const auto type = q.value(1).toString(), source = q.value(2).toString();
                const auto payload = QJsonDocument::fromJson(q.value(3).toByteArray()).object();
                if (type == "UserMessageReceived" && payload.contains("original_origin_ids"))
                    return R::failure(domainError("UNKNOWN_REFERENCE", "A session processing log is not an original memory source"));
                origin.occurredAt = QDateTime::fromString(q.value(4).toString(), Qt::ISODateWithMs);
                origin.originKind = originKindFromString(payload.value("origin_kind").toString())
                                        .value_or(OriginKind::Internal);
                if (!payload.contains("origin_kind") && type == "UserMessageReceived" &&
                    source == "AIBrain")
                    origin.originKind = OriginKind::LocalOwner;
                origin.conversationId = payload.value("conversation_id").toString();
                if (type == "UserMessageReceived" || (type == "MessageObserved" && source == "ConversationStore")) {
                    origin.speakerPersonId = payload.value("subject_id").toString();
                    origin.text = payload.value("text").toString();
                    if (origin.originKind == OriginKind::LocalOwner) {
                        if (origin.speakerPersonId.isEmpty())
                            origin.speakerPersonId = "owner";
                        if (origin.conversationId.isEmpty())
                            origin.conversationId = "local_owner";
                    }
                }
                const bool trustedReferences =
                    (type == "UserMessageReceived" && source == "AIBrain") ||
                    (type == "MessageObserved" && source == "ConversationStore");
                if (trustedReferences) {
                    for (const auto& value : payload.value("resolved_people").toArray()) {
                        const auto reference = value.toObject();
                        const auto role = reference.value("role").toString();
                        const auto person = reference.value("personId").toString();
                        if (!person.isEmpty() && (role == "subject" || role == "participant"))
                            origin.resolvedPeople.append(
                                {person, role == "subject" ? MemoryPersonRole::Subject
                                                           : MemoryPersonRole::Participant});
                    }
                }
                return R::success(origin);
            },
            [this](const QString& profile, const QString& person) -> Result<bool, DomainError> {
                QSqlQuery q(QSqlDatabase::database(m_eventRepository->connectionName()));
                q.prepare("SELECT 1 FROM agent_persons WHERE profile_id=? AND person_id=?");
                q.addBindValue(profile);
                q.addBindValue(person);
                if (!q.exec())
                    return Result<bool, DomainError>::failure(
                        domainError("STORE_UNAVAILABLE", q.lastError().text()));
                return Result<bool, DomainError>::success(q.next());
            });
    }

    bool persistCandidates(const DeferredChatSideEffect& effect) {
        auto service = attributionService();
        for (const auto& candidate : effect.memoryCandidates) {
            AttributedMemoryInput input;
            input.entry = candidate.entry;
            input.attribution = effect.attributionContext;
            input.explicitRequest = candidate.explicitRequest;
            if (candidate.operation == MemoryCandidateOperation::Write) {
                auto result = service.ingestMemory(input);
                if (!result)
                    return false;
                for (const auto& id : result.value().memoryIds)
                    m_changedMemoryIds.insert(id);
            } else {
                auto attribution = service.attribute(input);
                if (!attribution)
                    return false;
                const auto& origins = attribution.value().attribution.origins;
                if (origins.isEmpty() || !origins.first().committed ||
                    origins.first().speakerPersonId.isEmpty())
                    continue;
                if (!m_memoryStore->refreshDatabaseOnly())
                    return false;
                MemoryMutationBatch mutations;
                for (auto entry : m_memoryStore->all()) {
                    if (!MemoryPolicy::matchesForgetQuery(entry, candidate.query))
                        continue;
                    const bool localOwner =
                        effect.attributionContext.speakerPersonId == "owner" &&
                        effect.attributionContext.conversationId == "local_owner" &&
                        effect.attributionContext.audience.kind == MemoryAudienceKind::OwnerOnly;
                    if (!localOwner &&
                        !entry.attribution.subjects().contains(origins.first().speakerPersonId))
                        continue;
                    if (entry.attribution.audience.toJson() !=
                        effect.attributionContext.audience.toJson())
                        continue;
                    entry.status = MemoryStatus::Deleted;
                    if (!m_memoryStore->stageEntryUpdate(entry, &mutations))
                        return false;
                }
                if (!persistMemoryMutations(mutations))
                    return false;
            }
        }
        return true;
    }

    void migrateNextBatch() {
        if (m_closed || !m_memoryStore)
            return;
        const auto recovered = DaydreamConsolidator(*m_memoryStore).recoverMaintenance();
        if (!recovered) {
            if (warningReady) warningReady("Memory maintenance recovery failed; attribution migration deferred");
            return;
        }
        QStringList changed;
        for (const auto& source : recovered.value()) {
            changed.append(source.id);
            const auto current = m_memoryStore->readForRecall(source.id);
            if (current) {
                const auto target = current->payload.value("consolidated_into").toString();
                if (!target.isEmpty()) changed.append(target);
            }
        }
        if (!changed.isEmpty() && drainedReady) drainedReady(changed);
        const auto result =
            attributionService().migrateAttribution({m_environment.profileId, {}, 100});
        if (!result) {
            if (warningReady)
                warningReady("Memory attribution migration failed");
            return;
        }
        if (!result.value().complete)
            QTimer::singleShot(result.value().deferredForRecovery ? 1000 : 0,
                this, [this] { migrateNextBatch(); });
    }

    void discardPendingLocked() {
        const int discarded = m_state->pending.size();
        m_state->pending.clear();
        m_state->barriersAccepted.clear();
        m_state->depth -= discarded;
        m_state->totalDepth->fetch_sub(discarded, std::memory_order_acq_rel);
    }

    void report(const QString& phase, const QString& sessionId) {
        LifecycleProbe probe;
        {
            QMutexLocker lock(&m_probeState->mutex);
            probe = m_probeState->probe;
        }
        if (probe) {
            probe(phase, sessionId,
                  reinterpret_cast<quintptr>(QThread::currentThreadId()));
        }
    }

    ChatSideEffectEnvironment m_environment;
    std::shared_ptr<QueueState> m_state;
    std::shared_ptr<std::atomic_int> m_delayMs;
    std::shared_ptr<ProbeState> m_probeState;
    std::unique_ptr<EventSchemaRegistry> m_schemas;
    std::unique_ptr<SqliteEventRepository> m_eventRepository;
    std::unique_ptr<SqliteEventLedger> m_eventLedger;
    std::unique_ptr<MemoryStore> m_memoryStore;
    std::unique_ptr<AiCallLogger> m_logger;
    bool m_closed = false;
};

namespace {

bool isDroppableLog(ChatSideEffectType type) {
    return type == ChatSideEffectType::RequestLog
        || type == ChatSideEffectType::ResponseLog;
}

} // namespace

ChatSideEffectQueue::ChatSideEffectQueue(QObject* parent)
    : QObject(parent)
    , m_queueDepth(std::make_shared<std::atomic_int>(0))
    , m_activeWorkers(std::make_shared<std::atomic_int>(0))
    , m_testEffectDelayMs(std::make_shared<std::atomic_int>(0))
    , m_probeState(std::make_shared<ProbeState>()) {}

ChatSideEffectQueue::~ChatSideEffectQueue() {
    shutdownAndWait();
}

Result<void, DomainError> ChatSideEffectQueue::start(
    const ChatSideEffectEnvironment& environment) {
    if (environment.profileId.trimmed().isEmpty()
        || environment.runtimeDatabasePath.trimmed().isEmpty()
        || environment.memoryDatabasePath.trimmed().isEmpty()) {
        return Result<void, DomainError>::failure(domainError(
            QStringLiteral("CHAT_SIDE_EFFECT_UNAVAILABLE"),
            QStringLiteral("chat persistence environment is incomplete")));
    }
    if (m_thread && m_thread->isRunning()) {
        if (m_accepting.load(std::memory_order_acquire)) {
            return Result<void, DomainError>::success();
        }
        m_pendingRestartEnvironment = environment;
        return Result<void, DomainError>::success();
    }
    return startWorker(environment);
}

Result<void, DomainError> ChatSideEffectQueue::startWorker(
    const ChatSideEffectEnvironment& environment) {

    auto state = std::make_shared<QueueState>();
    state->totalDepth = m_queueDepth;
    state->maxDepth = qMax(1, environment.maxQueueDepth);
    auto* thread = new QThread;
    auto* worker = new Worker(environment, state, m_testEffectDelayMs,
                              m_probeState);
    worker->moveToThread(thread);
    QPointer<ChatSideEffectQueue> guard(this);
    worker->drainedReady = [guard](const QStringList& changed) {
        if (guard) QMetaObject::invokeMethod(guard, [guard, changed] {
            if (guard) emit guard->drained(changed);
        }, Qt::QueuedConnection);
    };
    worker->barrierReady = [guard](const QString& sessionId, quint64 generation) {
        if (!guard) return;
        QMetaObject::invokeMethod(
            guard, [guard, sessionId, generation]() {
                if (guard) emit guard->barrierCommitted(sessionId, generation);
            }, Qt::QueuedConnection);
    };
    worker->warningReady = [guard](const QString& message) {
        if (!guard) return;
        QMetaObject::invokeMethod(
            guard, [guard, message]() {
                if (guard) emit guard->persistenceWarning(message);
            }, Qt::QueuedConnection);
    };
    connect(thread, &QThread::finished, this,
            [this, thread]() { handleWorkerThreadFinished(thread); });
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    const auto activeWorkers = m_activeWorkers;
    connect(thread, &QThread::finished, [activeWorkers]() {
        activeWorkers->fetch_sub(1, std::memory_order_acq_rel);
    });
    m_activeWorkers->fetch_add(1, std::memory_order_acq_rel);
    thread->start();

    Result<void, DomainError> initialized = Result<void, DomainError>::failure(
        domainError(QStringLiteral("CHAT_SIDE_EFFECT_UNAVAILABLE"),
                    QStringLiteral("chat persistence worker did not start")));
    const bool invoked = QMetaObject::invokeMethod(
        worker, [worker, &initialized]() { initialized = worker->initialize(); },
        Qt::BlockingQueuedConnection);
    if (!invoked || !initialized.isOk()) {
        {
            QMutexLocker lock(&state->mutex);
            state->stopRequested = true;
            state->drainAcceptedWork = false;
        }
        QMetaObject::invokeMethod(worker, [worker]() { worker->shutdownAndQuit(); },
                                  Qt::QueuedConnection);
        return initialized;
    }

    {
        QMutexLocker lock(&state->mutex);
        state->accepting = true;
    }
    m_state = std::move(state);
    m_thread = thread;
    m_worker = worker;
    m_stopping = false;
    m_accepting.store(true, std::memory_order_release);
    return Result<void, DomainError>::success();
}

bool ChatSideEffectQueue::submitMemoryTask(
    std::function<QJsonObject(MemoryStore&)> task,
    std::function<void(QJsonObject)> completed) {
    DeferredChatSideEffect effect;
    effect.type = ChatSideEffectType::MemoryTask;
    effect.memoryTask = std::move(task);
    const QPointer<ChatSideEffectQueue> guard(this);
    effect.memoryTaskCompleted = [guard, completed = std::move(completed)](QJsonObject result) {
        if (!guard) return;
        QMetaObject::invokeMethod(guard, [guard, completed, result] {
            if (guard && completed) completed(result);
        }, Qt::QueuedConnection);
    };
    return tryEnqueue(std::move(effect));
}

bool ChatSideEffectQueue::tryEnqueue(DeferredChatSideEffect effect) {
    const auto state = m_state;
    Worker* const worker = m_worker.data();
    if (!m_accepting.load(std::memory_order_acquire) || !state || !worker) {
        emit persistenceWarning(QStringLiteral(
            "Chat persistence queue is not accepting work"));
        return false;
    }

    bool schedulePump = false;
    bool droppedLog = false;
    QString rejectionWarning;
    const bool incomingLog = isDroppableLog(effect.type);
    {
        QMutexLocker lock(&state->mutex);
        if (!state->accepting || state->stopRequested) {
            rejectionWarning = QStringLiteral("Chat persistence queue is not accepting work");
        }
        if (!effect.sessionId.isEmpty()
            && state->barriersAccepted.contains(effect.sessionId)) {
            rejectionWarning = QStringLiteral(
                "Chat persistence rejected work submitted after its session barrier");
        }

        const int capacity = effect.type == ChatSideEffectType::Barrier
            ? state->maxDepth : qMax(1, state->maxDepth - 1);
        if (rejectionWarning.isEmpty() && state->depth >= capacity) {
            if (incomingLog) {
                droppedLog = true;
            } else {
                const auto log = std::find_if(
                    state->pending.begin(), state->pending.end(),
                    [](const DeferredChatSideEffect& queued) {
                        return isDroppableLog(queued.type);
                    });
                if (log == state->pending.end()) {
                    rejectionWarning = QStringLiteral(
                        "Chat persistence queue is full; critical work was rejected");
                } else {
                    state->pending.erase(log);
                    --state->depth;
                    state->totalDepth->fetch_sub(1, std::memory_order_acq_rel);
                    droppedLog = true;
                }
            }
        }
        if (rejectionWarning.isEmpty() && (!incomingLog || !droppedLog)) {
            if (effect.type == ChatSideEffectType::Barrier
                && !effect.sessionId.isEmpty()) {
                state->barriersAccepted.insert(effect.sessionId);
            }
            state->pending.append(std::move(effect));
            ++state->depth;
            state->totalDepth->fetch_add(1, std::memory_order_acq_rel);
            if (!state->pumpScheduled) {
                state->pumpScheduled = true;
                schedulePump = true;
            }
        }
    }

    if (!rejectionWarning.isEmpty()) {
        emit persistenceWarning(rejectionWarning);
        return false;
    }

    if (droppedLog) {
        emit persistenceWarning(QStringLiteral(
            "Chat persistence queue is full; a non-critical log was dropped"));
        if (incomingLog) return false;
    }
    if (!schedulePump) return true;
    if (QMetaObject::invokeMethod(worker, [worker]() { worker->pump(); },
                                  Qt::QueuedConnection)) {
        return true;
    }

    int discarded = 0;
    {
        QMutexLocker lock(&state->mutex);
        discarded = state->pending.size();
        state->pending.clear();
        state->barriersAccepted.clear();
        state->depth -= discarded;
        state->totalDepth->fetch_sub(discarded, std::memory_order_acq_rel);
        state->accepting = false;
        state->pumpScheduled = false;
    }
    m_accepting.store(false, std::memory_order_release);
    emit persistenceWarning(QStringLiteral(
        "Chat persistence worker is unavailable"));
    return false;
}

void ChatSideEffectQueue::enqueue(DeferredChatSideEffect effect) {
    tryEnqueue(std::move(effect));
}

bool ChatSideEffectQueue::tryEnqueueBarrier(QString sessionId,
                                            quint64 generation) {
    DeferredChatSideEffect barrier;
    barrier.type = ChatSideEffectType::Barrier;
    barrier.requestId = sessionId;
    barrier.sessionId = std::move(sessionId);
    barrier.generation = generation;
    return tryEnqueue(std::move(barrier));
}

void ChatSideEffectQueue::enqueueBarrier(QString sessionId,
                                         quint64 generation) {
    tryEnqueueBarrier(std::move(sessionId), generation);
}

void ChatSideEffectQueue::stop(bool drainAcceptedWork) {
    m_accepting.store(false, std::memory_order_release);
    const auto state = m_state;
    if (!state)
        return;
    m_stopping = true;

    {
        QMutexLocker lock(&state->mutex);
        if (state->workerClosed)
            return;
        Worker *const worker = m_worker.data();
        if (!worker)
            return;
        state->accepting = false;
        state->stopRequested = true;
        state->drainAcceptedWork = drainAcceptedWork;
        if (!drainAcceptedWork) {
            const int discarded = state->pending.size();
            state->pending.clear();
            state->barriersAccepted.clear();
            state->depth -= discarded;
            state->totalDepth->fetch_sub(discarded, std::memory_order_acq_rel);
        }
        if (!state->pumpScheduled) {
            state->pumpScheduled = true;
            // Shutdown publishes workerClosed under the same mutex before
            // deletion. Keep the short queued post inside this lifetime fence.
            QMetaObject::invokeMethod(
                worker, [worker]() { worker->pump(); }, Qt::QueuedConnection);
        }
    }
}

void ChatSideEffectQueue::handleWorkerThreadFinished(QThread* thread) {
    if (m_thread != thread) return;
    m_state.reset();
    m_worker = nullptr;
    m_thread = nullptr;
    m_stopping = false;
    if (m_destroying || !m_pendingRestartEnvironment.has_value()) return;
    const ChatSideEffectEnvironment environment =
        std::move(*m_pendingRestartEnvironment);
    m_pendingRestartEnvironment.reset();
    const auto restarted = startWorker(environment);
    if (!restarted.isOk()) {
        emit persistenceWarning(QStringLiteral(
            "Chat persistence worker could not restart"));
    }
}

void ChatSideEffectQueue::shutdownAndWait() {
    m_destroying = true;
    m_pendingRestartEnvironment.reset();
    stop(false);
    QThread* const thread = m_thread.data();
    if (thread && thread->isRunning()) {
        thread->requestInterruption();
        thread->wait(1000);
    }
    m_state.reset();
    m_worker = nullptr;
    m_thread = nullptr;
}

#ifdef DESKTOP_PET_ENABLE_TEST_SEAMS
void ChatSideEffectQueue::setTestLifecycleProbe(LifecycleProbe probe) {
    QMutexLocker lock(&m_probeState->mutex);
    m_probeState->probe = std::move(probe);
}

void ChatSideEffectQueue::setTestEffectDelayMs(int delayMs) {
    m_testEffectDelayMs->store(qMax(0, delayMs), std::memory_order_release);
}

bool ChatSideEffectQueue::isWorkerThreadRunningForTests() const {
    return m_activeWorkers->load(std::memory_order_acquire) > 0;
}
#endif
