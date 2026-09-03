#include "chat_side_effect_queue.h"

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
        m_memoryStore->setDatabasePath(m_environment.memoryDatabasePath);
        m_memoryStore->setStoragePath(QString());
        QString memoryError;
        if (!m_memoryStore->loadDatabaseOnly(&memoryError)) {
            return Result<void, DomainError>::failure(domainError(
                QStringLiteral("MEMORY_STORE_UNAVAILABLE"), memoryError));
        }
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
            if (!hasEffect) return;
            process(std::move(effect));
        }
    }

    void shutdownAndQuit() {
        if (m_closed) return;
        m_closed = true;
        m_logger.reset();
        m_memoryStore.reset();
        m_eventLedger.reset();
        if (m_eventRepository) m_eventRepository->close();
        m_eventRepository.reset();
        m_schemas.reset();
        report(QStringLiteral("worker.closed"), QString());
        QThread::currentThread()->quit();
    }

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
            ok = persistReinforcement(effect.reinforcedEntries, effect.sessionId);
            completionPhase = QStringLiteral("memory.reinforcement.completed");
            break;
        case ChatSideEffectType::UserMemoryWrite:
            ok = persistMemoryMutations(effect.memoryMutations);
            completionPhase = QStringLiteral("user.memory.write.completed");
            break;
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

    bool appendEvent(const EventDraft& event) {
        if (!m_eventLedger) return false;
        auto result = m_eventLedger->append(event);
        if (result.isOk()) return true;
        const QString message = result.error().message.toLower();
        if (!message.contains(QStringLiteral("busy"))
            && !message.contains(QStringLiteral("locked"))) {
            return false;
        }
        QThread::msleep(5);
        return m_eventLedger->append(event).isOk();
    }

    bool persistReinforcement(const QList<MemoryEntry>& stagedEntries,
                              const QString& sessionId) {
        if (persistReinforcementOnce(stagedEntries, sessionId)) return true;
        QThread::msleep(5);
        return m_memoryStore && m_memoryStore->refreshDatabaseOnly()
            && persistReinforcementOnce(stagedEntries, sessionId);
    }

    bool persistReinforcementOnce(const QList<MemoryEntry>& stagedEntries,
                                  const QString& sessionId) {
        if (!m_memoryStore || stagedEntries.isEmpty()) return true;
        if (!m_memoryStore->refreshDatabaseOnly()) return false;

        QList<MemoryEntry> updates;
        QSet<QString> seen;
        for (const MemoryEntry& staged : stagedEntries) {
            if (staged.id.trimmed().isEmpty() || seen.contains(staged.id)) continue;
            seen.insert(staged.id);
            const MemoryEntry* persisted = m_memoryStore->findById(staged.id);
            if (!persisted || persisted->status != MemoryStatus::Active) continue;
            MemoryEntry updated = *persisted;
            updated.strength = qMin(1.0, updated.strength + 0.1);
            updated.accessCount += 1;
            updated.lastAccessedAt = staged.lastAccessedAt.isValid()
                ? staged.lastAccessedAt.toUTC() : QDateTime::currentDateTimeUtc();
            updated.updatedAt = updated.lastAccessedAt;
            updates.append(std::move(updated));
        }
        if (updates.isEmpty()) return true;
        if (!m_memoryStore->beginTransaction()) return false;
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

    bool persistMemoryMutations(const MemoryMutationBatch& mutations) {
        if (!m_memoryStore || mutations.isEmpty()) return true;
        if (m_memoryStore->persistMutationBatch(mutations)) return true;
        QThread::msleep(5);
        return m_memoryStore->refreshDatabaseOnly()
            && m_memoryStore->persistMutationBatch(mutations);
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
    Worker* const worker = m_worker.data();
    if (!state || !worker) return;
    m_stopping = true;

    bool schedulePump = false;
    {
        QMutexLocker lock(&state->mutex);
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
            schedulePump = true;
        }
    }
    if (schedulePump) {
        QMetaObject::invokeMethod(worker, [worker]() { worker->pump(); },
                                  Qt::QueuedConnection);
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
