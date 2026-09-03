#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QMutex>
#include <QMutexLocker>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

#include <atomic>

#include "ai/chat/chat_side_effect_queue.h"
#include "ai/memory/memory_store.h"
#include "ai/memory/sqlite_memory_repository.h"

namespace {

const QString kProfileId = QStringLiteral(
    "11111111-1111-4111-8111-111111111111");

ChatSideEffectEnvironment environmentFor(const QTemporaryDir& directory) {
    ChatSideEffectEnvironment environment;
    environment.profileId = kProfileId;
    environment.runtimeDatabasePath =
        directory.filePath(QStringLiteral("agent_runtime.sqlite"));
    environment.memoryDatabasePath =
        directory.filePath(QStringLiteral("memory.db"));
    environment.aiCallLogPath =
        directory.filePath(QStringLiteral("ai_calls.jsonl"));
    return environment;
}

MemoryEntry memoryFor(int index) {
    MemoryEntry entry;
    entry.id = QStringLiteral("memory-%1").arg(index);
    entry.type = MemoryType::Preference;
    entry.status = MemoryStatus::Active;
    entry.key = QStringLiteral("preference:%1").arg(index);
    entry.summary = QStringLiteral("memory %1").arg(index);
    entry.content = entry.summary;
    entry.strength = 0.4;
    entry.confidence = 0.9;
    entry.createdAt = QDateTime::currentDateTimeUtc();
    entry.updatedAt = entry.createdAt;
    return entry;
}

DeferredChatSideEffect eventEffect(const QString& type,
                                   const QString& sessionId,
                                   quint64 generation) {
    DeferredChatSideEffect effect;
    effect.type = ChatSideEffectType::RuntimeEvent;
    effect.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    effect.sessionId = sessionId;
    effect.generation = generation;
    effect.event.profileId = kProfileId;
    effect.event.type = type;
    effect.event.source = QStringLiteral("test");
    effect.event.sessionId = sessionId;
    if (type == QLatin1String("UserMessageReceived")) {
        effect.event.payload = {
            {QStringLiteral("text"), QStringLiteral("hello")},
            {QStringLiteral("triggerTag"), QStringLiteral("user_request")}
        };
    } else if (type == QLatin1String("ModelCallCompleted")) {
        effect.event.payload = {
            {QStringLiteral("role"), QStringLiteral("dialogue")},
            {QStringLiteral("success"), true},
            {QStringLiteral("durationMs"), 12.0},
            {QStringLiteral("provider"), QStringLiteral("test")},
            {QStringLiteral("model"), QStringLiteral("test-model")},
            {QStringLiteral("promptTokens"), 1},
            {QStringLiteral("completionTokens"), 1},
            {QStringLiteral("totalTokens"), 2}
        };
    } else {
        effect.event.payload = {
            {QStringLiteral("text"), QStringLiteral("world")},
            {QStringLiteral("triggerTag"), QStringLiteral("user_request")}
        };
    }
    return effect;
}

QStringList storedEventTypes(const QString& databasePath,
                             const QString& sessionId) {
    const QString connectionName = QStringLiteral("side_effect_test_")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QStringList types;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(databasePath);
        if (database.open()) {
            QSqlQuery query(database);
            query.prepare(QStringLiteral(
                "SELECT type FROM event_log WHERE session_id=? ORDER BY sequence"));
            query.addBindValue(sessionId);
            if (query.exec()) {
                while (query.next()) types.append(query.value(0).toString());
            }
            database.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return types;
}

QList<QJsonObject> storedLogRecords(const QString& path) {
    QList<QJsonObject> records;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return records;
    while (!file.atEnd()) {
        const QJsonDocument document = QJsonDocument::fromJson(file.readLine());
        if (document.isObject()) records.append(document.object());
    }
    return records;
}

int storedMemoryCount(const QString& databasePath) {
    SQLiteMemoryRepository repository;
    QString error;
    if (!repository.open(databasePath, &error)) return -1;
    return repository.loadAll().size();
}

DeferredChatSideEffect logEffect(ChatSideEffectType type,
                                 const QString& requestId,
                                 const QString& sessionId,
                                 quint64 generation) {
    DeferredChatSideEffect effect;
    effect.type = type;
    effect.requestId = requestId;
    effect.sessionId = sessionId;
    effect.generation = generation;
    effect.logRecord = {
        {QStringLiteral("type"),
         type == ChatSideEffectType::RequestLog
             ? QStringLiteral("request") : QStringLiteral("response")},
        {QStringLiteral("request_id"), requestId}
    };
    return effect;
}

} // namespace

class ChatSideEffectQueueTests : public QObject {
    Q_OBJECT

private slots:
    void start_whenEnvironmentIsValid_shouldOpenThreadOwnedPersistenceResources();
    void start_whenOptionalLogPathIsUnavailable_shouldKeepDatabaseEffectsAvailable();
    void start_whenLegacyJsonExists_shouldNotImportItIntoWorkerDatabase();
    void start_whenDatabaseOpenFails_shouldReleaseWorkerResources();
    void enqueue_whenSessionHasOrderedEffects_shouldPersistBeforeBarrier();
    void enqueue_whenReinforcementContainsEightEntries_shouldUseOneTransaction();
    void enqueue_whenReinforcementSnapshotWasDeleted_shouldNotReactivateEntry();
    void enqueue_whenLogWriteFails_shouldWarnWithoutBlockingLaterEffects();
    void enqueue_whenQueueIsFull_shouldDropLogsBeforeCriticalEffects();
    void enqueue_whenBarrierWasAccepted_shouldRejectLaterSessionEffects();
    void stop_whenWorkIsPending_shouldReturnWithoutWaitingAndSettleDepth();
    void stop_whenStartedImmediately_shouldCloseOldWorkerBeforeOpeningNewWorker();
    void destructor_whenWorkIsPending_shouldCloseWorkerBeforeReturning();
};

void ChatSideEffectQueueTests::
start_whenEnvironmentIsValid_shouldOpenThreadOwnedPersistenceResources() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
    store.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(store.load());
    ChatSideEffectQueue queue;
    QList<QPair<QString, quintptr>> lifecycle;
    queue.setTestLifecycleProbe(
        [&lifecycle](const QString& phase, const QString&, quintptr threadId) {
            lifecycle.append({phase, threadId});
        });

    QVERIFY(queue.start(environmentFor(directory)).isOk());
    queue.stop(true);
    QTRY_VERIFY_WITH_TIMEOUT(!queue.isWorkerThreadRunningForTests(), 1000);

    QVERIFY(std::any_of(lifecycle.cbegin(), lifecycle.cend(), [](const auto& item) {
        return item.first == QLatin1String("memory.open");
    }));
    QVERIFY(std::any_of(lifecycle.cbegin(), lifecycle.cend(), [](const auto& item) {
        return item.first == QLatin1String("event.open");
    }));
    QVERIFY(std::all_of(lifecycle.cbegin(), lifecycle.cend(),
                        [&lifecycle](const auto& item) {
                            return item.second == lifecycle.first().second;
                        }));
    QVERIFY(lifecycle.first().second
            != reinterpret_cast<quintptr>(QThread::currentThreadId()));
}

void ChatSideEffectQueueTests::
start_whenOptionalLogPathIsUnavailable_shouldKeepDatabaseEffectsAvailable() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
    store.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(store.load());
    ChatSideEffectEnvironment environment = environmentFor(directory);
    environment.aiCallLogPath = directory.path();
    ChatSideEffectQueue queue;
    QVERIFY(queue.start(environment).isOk());

    const QString sessionId = QStringLiteral("session-log-unavailable");
    queue.enqueue(eventEffect(QStringLiteral("UserMessageReceived"), sessionId, 1));
    QSignalSpy barrier(&queue, &ChatSideEffectQueue::barrierCommitted);
    queue.enqueueBarrier(sessionId, 1);
    QTRY_COMPARE_WITH_TIMEOUT(barrier.size(), 1, 1000);
    QCOMPARE(storedEventTypes(environment.runtimeDatabasePath, sessionId),
             QStringList({QStringLiteral("UserMessageReceived")}));
}

void ChatSideEffectQueueTests::
start_whenLegacyJsonExists_shouldNotImportItIntoWorkerDatabase() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("log")));
    QFile legacy(directory.filePath(QStringLiteral("log/ai_memory.json")));
    QVERIFY(legacy.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const MemoryEntry legacyEntry = memoryFor(91);
    legacy.write(QJsonDocument(QJsonArray{legacyEntry.toJson()}).toJson());
    legacy.close();

    const QString previousDirectory = QDir::currentPath();
    QVERIFY(QDir::setCurrent(directory.path()));
    ChatSideEffectQueue queue;
    const ChatSideEffectEnvironment environment = environmentFor(directory);
    const auto started = queue.start(environment);
    QVERIFY(QDir::setCurrent(previousDirectory));
    QVERIFY(started.isOk());
    QSignalSpy barrier(&queue, &ChatSideEffectQueue::barrierCommitted);
    queue.enqueueBarrier(QStringLiteral("legacy-isolation"), 1);
    QTRY_COMPARE_WITH_TIMEOUT(barrier.size(), 1, 1000);

    QCOMPARE(storedMemoryCount(environment.memoryDatabasePath), 0);
}

void ChatSideEffectQueueTests::
start_whenDatabaseOpenFails_shouldReleaseWorkerResources() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ChatSideEffectEnvironment environment = environmentFor(directory);
    environment.runtimeDatabasePath = directory.path();
    ChatSideEffectQueue queue;
    std::atomic_int closed{0};
    queue.setTestLifecycleProbe(
        [&closed](const QString& phase, const QString&, quintptr) {
            if (phase == QLatin1String("worker.closed")) ++closed;
        });

    QVERIFY(!queue.start(environment).isOk());
    QTRY_COMPARE_WITH_TIMEOUT(closed.load(), 1, 1000);
    QVERIFY(!queue.isAccepting());
    QCOMPARE(queue.queueDepth(), 0);
}

void ChatSideEffectQueueTests::
enqueue_whenSessionHasOrderedEffects_shouldPersistBeforeBarrier() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
    store.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(store.load());
    const ChatSideEffectEnvironment environment = environmentFor(directory);
    ChatSideEffectQueue queue;
    QVERIFY(queue.start(environment).isOk());
    const QString sessionId = QStringLiteral("session-ordered");
    QVERIFY(queue.tryEnqueue(
        eventEffect(QStringLiteral("UserMessageReceived"), sessionId, 4)));
    QVERIFY(queue.tryEnqueue(logEffect(ChatSideEffectType::RequestLog,
                                       QStringLiteral("request-1"), sessionId, 4)));
    QVERIFY(queue.tryEnqueue(
        eventEffect(QStringLiteral("ModelCallCompleted"), sessionId, 4)));
    QVERIFY(queue.tryEnqueue(
        eventEffect(QStringLiteral("AssistantResponseProduced"), sessionId, 4)));
    QVERIFY(queue.tryEnqueue(logEffect(ChatSideEffectType::ResponseLog,
                                       QStringLiteral("request-1"), sessionId, 4)));
    QSignalSpy barrier(&queue, &ChatSideEffectQueue::barrierCommitted);
    QVERIFY(queue.tryEnqueueBarrier(sessionId, 4));

    QTRY_COMPARE_WITH_TIMEOUT(barrier.size(), 1, 1000);
    QCOMPARE(storedEventTypes(environment.runtimeDatabasePath, sessionId),
             QStringList({QStringLiteral("UserMessageReceived"),
                          QStringLiteral("ModelCallCompleted"),
                          QStringLiteral("AssistantResponseProduced")}));
    const QList<QJsonObject> logs = storedLogRecords(environment.aiCallLogPath);
    QCOMPARE(logs.size(), 2);
    QCOMPARE(logs.at(0).value(QStringLiteral("type")).toString(),
             QStringLiteral("request"));
    QCOMPARE(logs.at(1).value(QStringLiteral("type")).toString(),
             QStringLiteral("response"));
    QCOMPARE(logs.at(0).value(QStringLiteral("request_id")).toString(),
             logs.at(1).value(QStringLiteral("request_id")).toString());
}

void ChatSideEffectQueueTests::
enqueue_whenReinforcementSnapshotWasDeleted_shouldNotReactivateEntry() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
    store.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(store.load());
    const MemoryEntry stored = store.addEntry(memoryFor(1));
    const MemoryReinforcementBatch batch = store.stageReinforcement({stored.id});
    QVERIFY(store.updateStatusById(stored.id, MemoryStatus::Deleted));

    ChatSideEffectQueue queue;
    const ChatSideEffectEnvironment environment = environmentFor(directory);
    QVERIFY(queue.start(environment).isOk());
    DeferredChatSideEffect reinforce;
    reinforce.type = ChatSideEffectType::MemoryReinforcement;
    reinforce.requestId = QStringLiteral("deleted-reinforcement");
    reinforce.generation = 3;
    reinforce.sessionId = QStringLiteral("deleted-session");
    reinforce.reinforcedEntries = batch.entries;
    QVERIFY(queue.tryEnqueue(std::move(reinforce)));
    QSignalSpy barrier(&queue, &ChatSideEffectQueue::barrierCommitted);
    QVERIFY(queue.tryEnqueueBarrier(QStringLiteral("deleted-session"), 3));
    QTRY_COMPARE_WITH_TIMEOUT(barrier.size(), 1, 1000);

    MemoryStore reloaded;
    reloaded.setDatabasePath(environment.memoryDatabasePath);
    reloaded.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(reloaded.loadDatabaseOnly());
    const MemoryEntry* entry = reloaded.findById(stored.id);
    QVERIFY(entry);
    QCOMPARE(entry->status, MemoryStatus::Deleted);
    QCOMPARE(entry->accessCount, 0);
}

void ChatSideEffectQueueTests::
enqueue_whenReinforcementContainsEightEntries_shouldUseOneTransaction() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
    store.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(store.load());
    QStringList ids;
    for (int i = 0; i < 8; ++i) ids.append(store.addEntry(memoryFor(i)).id);
    const MemoryReinforcementBatch batch = store.stageReinforcement(ids);
    ChatSideEffectQueue queue;
    QVERIFY(queue.start(environmentFor(directory)).isOk());
    std::atomic_int commits{0};
    queue.setTestLifecycleProbe(
        [&commits](const QString& phase, const QString&, quintptr) {
            if (phase == QLatin1String("memory.transaction.commit")) ++commits;
        });
    DeferredChatSideEffect effect;
    effect.type = ChatSideEffectType::MemoryReinforcement;
    effect.requestId = QStringLiteral("reinforce-eight");
    effect.generation = 2;
    effect.sessionId = QStringLiteral("session-eight");
    effect.reinforcedEntries = batch.entries;
    queue.enqueue(std::move(effect));
    QSignalSpy barrier(&queue, &ChatSideEffectQueue::barrierCommitted);
    queue.enqueueBarrier(QStringLiteral("session-eight"), 2);

    QTRY_COMPARE_WITH_TIMEOUT(barrier.size(), 1, 1000);
    QCOMPARE(commits.load(), 1);
    MemoryStore reloaded;
    reloaded.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
    reloaded.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(reloaded.load());
    for (const QString& id : ids) QCOMPARE(reloaded.findById(id)->accessCount, 1);
}

void ChatSideEffectQueueTests::
enqueue_whenLogWriteFails_shouldWarnWithoutBlockingLaterEffects() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
    store.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(store.load());
    const MemoryEntry stored = store.addEntry(memoryFor(1));
    const MemoryReinforcementBatch batch = store.stageReinforcement({stored.id});
    ChatSideEffectEnvironment environment = environmentFor(directory);
    environment.aiCallLogPath = directory.path();
    ChatSideEffectQueue queue;
    QVERIFY(queue.start(environment).isOk());
    QSignalSpy warnings(&queue, &ChatSideEffectQueue::persistenceWarning);
    DeferredChatSideEffect log;
    log.type = ChatSideEffectType::RequestLog;
    log.requestId = QStringLiteral("bad-log");
    log.logRecord = {{QStringLiteral("type"), QStringLiteral("request")}};
    queue.enqueue(std::move(log));
    DeferredChatSideEffect reinforce;
    reinforce.type = ChatSideEffectType::MemoryReinforcement;
    reinforce.requestId = QStringLiteral("after-bad-log");
    reinforce.reinforcedEntries = batch.entries;
    queue.enqueue(std::move(reinforce));
    QSignalSpy barrier(&queue, &ChatSideEffectQueue::barrierCommitted);
    queue.enqueueBarrier(QStringLiteral("after-log"), 1);

    QTRY_COMPARE_WITH_TIMEOUT(barrier.size(), 1, 1000);
    QVERIFY(!warnings.isEmpty());
    MemoryStore reloaded;
    reloaded.setDatabasePath(environment.memoryDatabasePath);
    reloaded.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(reloaded.load());
    QCOMPARE(reloaded.findById(stored.id)->accessCount, 1);
}

void ChatSideEffectQueueTests::
enqueue_whenQueueIsFull_shouldDropLogsBeforeCriticalEffects() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
    store.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(store.load());
    ChatSideEffectEnvironment environment = environmentFor(directory);
    environment.maxQueueDepth = 4;
    ChatSideEffectQueue queue;
    QVERIFY(queue.start(environment).isOk());
    queue.setTestEffectDelayMs(100);
    QSignalSpy warnings(&queue, &ChatSideEffectQueue::persistenceWarning);
    const QString sessionId = QStringLiteral("bounded-session");
    QVERIFY(queue.tryEnqueue(logEffect(ChatSideEffectType::RequestLog,
                                       QStringLiteral("log-1"), sessionId, 6)));
    QVERIFY(queue.tryEnqueue(logEffect(ChatSideEffectType::ResponseLog,
                                       QStringLiteral("log-2"), sessionId, 6)));
    QVERIFY(queue.tryEnqueue(logEffect(ChatSideEffectType::RequestLog,
                                       QStringLiteral("log-3"), sessionId, 6)));
    QVERIFY(queue.tryEnqueue(
        eventEffect(QStringLiteral("UserMessageReceived"), sessionId, 6)));
    QVERIFY(queue.tryEnqueue(
        eventEffect(QStringLiteral("AssistantResponseProduced"), sessionId, 6)));
    QVERIFY(queue.tryEnqueueBarrier(sessionId, 6));

    QSignalSpy barrier(&queue, &ChatSideEffectQueue::barrierCommitted);
    QTRY_COMPARE_WITH_TIMEOUT(barrier.size(), 1, 2000);
    QVERIFY(!warnings.isEmpty());
    QCOMPARE(storedEventTypes(environment.runtimeDatabasePath, sessionId),
             QStringList({QStringLiteral("UserMessageReceived"),
                          QStringLiteral("AssistantResponseProduced")}));
    QVERIFY(storedLogRecords(environment.aiCallLogPath).size() <= 1);
    QCOMPARE(queue.queueDepth(), 0);
}

void ChatSideEffectQueueTests::
enqueue_whenBarrierWasAccepted_shouldRejectLaterSessionEffects() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
    store.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(store.load());
    ChatSideEffectQueue queue;
    QVERIFY(queue.start(environmentFor(directory)).isOk());
    const QString sessionId = QStringLiteral("closed-session");
    QVERIFY(queue.tryEnqueueBarrier(sessionId, 8));
    QVERIFY(!queue.tryEnqueue(
        eventEffect(QStringLiteral("UserMessageReceived"), sessionId, 8)));
}

void ChatSideEffectQueueTests::
stop_whenWorkIsPending_shouldReturnWithoutWaitingAndSettleDepth() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
    store.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(store.load());
    ChatSideEffectQueue queue;
    QVERIFY(queue.start(environmentFor(directory)).isOk());
    queue.setTestEffectDelayMs(150);
    std::atomic_int closed{0};
    queue.setTestLifecycleProbe(
        [&closed](const QString& phase, const QString&, quintptr) {
            if (phase == QLatin1String("worker.closed")) ++closed;
        });
    queue.enqueue(eventEffect(QStringLiteral("UserMessageReceived"),
                              QStringLiteral("stop-pending"), 1));
    queue.enqueue(eventEffect(QStringLiteral("AssistantResponseProduced"),
                              QStringLiteral("stop-pending"), 1));
    QCOMPARE(queue.queueDepth(), 2);
    QElapsedTimer elapsed;
    elapsed.start();
    queue.stop(false);

    QVERIFY2(elapsed.elapsed() < 50, qPrintable(QString::number(elapsed.elapsed())));
    QTRY_COMPARE_WITH_TIMEOUT(queue.queueDepth(), 0, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(closed.load(), 1, 1000);
}

void ChatSideEffectQueueTests::
stop_whenStartedImmediately_shouldCloseOldWorkerBeforeOpeningNewWorker() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
    store.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(store.load());
    ChatSideEffectQueue queue;
    QMutex lifecycleMutex;
    QStringList lifecycle;
    queue.setTestLifecycleProbe(
        [&lifecycleMutex, &lifecycle](const QString& phase, const QString&, quintptr) {
            QMutexLocker lock(&lifecycleMutex);
            if (phase == QLatin1String("memory.open")
                || phase == QLatin1String("worker.closed")) {
                lifecycle.append(phase);
            }
        });
    const ChatSideEffectEnvironment environment = environmentFor(directory);
    QVERIFY(queue.start(environment).isOk());
    queue.setTestEffectDelayMs(80);
    QVERIFY(queue.tryEnqueue(eventEffect(QStringLiteral("UserMessageReceived"),
                                         QStringLiteral("restart-session"), 1)));

    queue.stop(true);
    QVERIFY(queue.start(environment).isOk());
    QTRY_VERIFY_WITH_TIMEOUT(([&lifecycleMutex, &lifecycle]() {
        QMutexLocker lock(&lifecycleMutex);
        return lifecycle.count(QStringLiteral("memory.open")) == 2;
    }()), 2000);
    queue.stop(true);
    QTRY_VERIFY_WITH_TIMEOUT(!queue.isWorkerThreadRunningForTests(), 2000);

    QMutexLocker lock(&lifecycleMutex);
    QCOMPARE(lifecycle.size(), 4);
    QCOMPARE(lifecycle.at(0), QStringLiteral("memory.open"));
    QCOMPARE(lifecycle.at(1), QStringLiteral("worker.closed"));
    QCOMPARE(lifecycle.at(2), QStringLiteral("memory.open"));
    QCOMPARE(lifecycle.at(3), QStringLiteral("worker.closed"));
}

void ChatSideEffectQueueTests::
destructor_whenWorkIsPending_shouldCloseWorkerBeforeReturning() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
    store.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(store.load());
    std::atomic_int closed{0};
    {
        ChatSideEffectQueue queue;
        queue.setTestLifecycleProbe(
            [&closed](const QString& phase, const QString&, quintptr) {
                if (phase == QLatin1String("worker.closed")) ++closed;
            });
        QVERIFY(queue.start(environmentFor(directory)).isOk());
        queue.setTestEffectDelayMs(250);
        QVERIFY(queue.tryEnqueue(eventEffect(QStringLiteral("UserMessageReceived"),
                                             QStringLiteral("destructor-session"), 1)));
    }

    QCOMPARE(closed.load(), 1);
}

QTEST_MAIN(ChatSideEffectQueueTests)
#include "test_chat_side_effect_queue.moc"
