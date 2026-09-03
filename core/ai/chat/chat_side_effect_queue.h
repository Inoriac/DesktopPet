#ifndef DESKTOP_PET_CHAT_SIDE_EFFECT_QUEUE_H
#define DESKTOP_PET_CHAT_SIDE_EFFECT_QUEUE_H

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QList>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include "ai/domain/domain_result.h"
#include "ai/event/event_types.h"
#include "ai/memory/memory_store.h"

class QThread;

enum class ChatSideEffectType {
    RuntimeEvent,
    MemoryReinforcement,
    UserMemoryWrite,
    RequestLog,
    ResponseLog,
    Barrier
};

struct ChatSideEffectEnvironment {
    QString profileId;
    QString runtimeDatabasePath;
    QString memoryDatabasePath;
    QString aiCallLogPath;
    int maxQueueDepth = 256;
};

struct DeferredChatSideEffect {
    ChatSideEffectType type = ChatSideEffectType::RuntimeEvent;
    QString requestId;
    quint64 generation = 0;
    QString sessionId;
    EventDraft event;
    QList<MemoryEntry> reinforcedEntries;
    MemoryMutationBatch memoryMutations;
    QJsonObject logRecord;
};

struct ChatPreparationTimings {
    qint64 uiAcknowledgeMs = 0;
    qint64 preparationMs = 0;
    qint64 dispatchLagMs = 0;
    int sideEffectQueueDepth = 0;
};

class ChatSideEffectQueue : public QObject {
    Q_OBJECT

public:
    using LifecycleProbe = std::function<void(
        const QString& phase, const QString& sessionId, quintptr threadId)>;

    explicit ChatSideEffectQueue(QObject* parent = nullptr);
    ~ChatSideEffectQueue() override;

    Result<void, DomainError> start(const ChatSideEffectEnvironment& environment);
    bool tryEnqueue(DeferredChatSideEffect effect);
    void enqueue(DeferredChatSideEffect effect);
    bool tryEnqueueBarrier(QString sessionId, quint64 generation);
    void enqueueBarrier(QString sessionId, quint64 generation);
    void stop(bool drainAcceptedWork);

    bool isAccepting() const { return m_accepting.load(); }
    int queueDepth() const { return m_queueDepth->load(); }

#ifdef DESKTOP_PET_ENABLE_TEST_SEAMS
    void setTestLifecycleProbe(LifecycleProbe probe);
    void setTestEffectDelayMs(int delayMs);
    bool isWorkerThreadRunningForTests() const;
#endif

signals:
    void barrierCommitted(const QString& sessionId, quint64 generation);
    void persistenceWarning(const QString& safeMessage);

private:
    struct ProbeState;
    struct QueueState;
    class Worker;
    Result<void, DomainError> startWorker(
        const ChatSideEffectEnvironment& environment);
    void handleWorkerThreadFinished(QThread* thread);
    void shutdownAndWait();

    QPointer<QThread> m_thread;
    QPointer<Worker> m_worker;
    std::shared_ptr<QueueState> m_state;
    std::shared_ptr<std::atomic_int> m_queueDepth;
    std::shared_ptr<std::atomic_int> m_activeWorkers;
    std::shared_ptr<std::atomic_int> m_testEffectDelayMs;
    std::shared_ptr<ProbeState> m_probeState;
    std::atomic_bool m_accepting{false};
    bool m_stopping = false;
    bool m_destroying = false;
    std::optional<ChatSideEffectEnvironment> m_pendingRestartEnvironment;
};

Q_DECLARE_METATYPE(DeferredChatSideEffect)
Q_DECLARE_METATYPE(ChatPreparationTimings)

#endif // DESKTOP_PET_CHAT_SIDE_EFFECT_QUEUE_H
