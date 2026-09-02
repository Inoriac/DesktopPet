#include <QtTest>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <optional>
#include <atomic>

#include "ai/chat/chat_preparation_executor.h"
#include "ai/event/runtime_unit_of_work.h"
#include "ai/event/sqlite_event_repository.h"
#include "ai/identity/sqlite_identity_repository.h"
#include "ai/memory/memory_relation.h"
#include "ai/memory/memory_retriever.h"
#include "ai/memory/memory_store.h"

namespace {

ChatPreparationEnvironment environmentFor(const QTemporaryDir& directory) {
    ChatPreparationEnvironment environment;
    environment.profileId = QStringLiteral("profile-test");
    environment.runtimeDatabasePath =
        directory.filePath(QStringLiteral("agent_runtime.sqlite"));
    environment.memoryDatabasePath = directory.filePath(QStringLiteral("memory.db"));
    environment.identityBaseline = IdentityBaseline::defaults();
    environment.personalityPolicy = PersonalityPolicy{};
    return environment;
}

ChatPreparationRequest requestFor(const QString& reason = QStringLiteral("我喜欢爵士乐")) {
    ChatPreparationRequest request;
    request.requestId = QStringLiteral("request-1");
    request.generation = 7;
    request.sessionId = QStringLiteral("session-1");
    request.reason = reason;
    request.triggerTag = QStringLiteral("user_request");
    request.petName = QStringLiteral("Milltina");
    request.runtimeMetadata.profileId = QStringLiteral("profile-test");
    request.runtimeMetadata.subjectId = QStringLiteral("owner");
    request.identityBaseline = IdentityBaseline::defaults();
    request.personalityPolicy = PersonalityPolicy{};
    return request;
}

MemoryEntry matchingMemory() {
    MemoryEntry entry;
    entry.id = QStringLiteral("memory-jazz");
    entry.type = MemoryType::Preference;
    entry.status = MemoryStatus::Active;
    entry.summary = QStringLiteral("主人喜欢爵士乐");
    entry.content = entry.summary;
    entry.importance = 0.9;
    entry.strength = 0.8;
    entry.confidence = 0.9;
    entry.createdAt = QDateTime::currentDateTimeUtc();
    entry.updatedAt = entry.createdAt;
    return entry;
}

bool prepareOnce(ChatPreparationExecutor& executor,
                 ChatPreparationRequest request,
                 ChatPreparationResult* output) {
    bool received = false;
    QObject::connect(&executor, &ChatPreparationExecutor::prepared, &executor,
                     [&received, output](ChatPreparationResult result) {
                         *output = std::move(result);
                         received = true;
                     });
    executor.submit(std::move(request));
    QElapsedTimer timer;
    timer.start();
    while (!received && timer.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QTest::qWait(1);
    }
    return received;
}

} // namespace

class ChatPreparationExecutorTests : public QObject {
    Q_OBJECT

private slots:
    void start_whenEnvironmentIsValid_shouldCreateWorkerOwnedResources();
    void start_whenDatabasePathsAreInvalid_shouldReturnControlledFailure();
    void submit_whenContextIsValid_shouldReturnMessagesAndReinforcementIds();
    void submit_whenIdentityDatabaseIsUnavailable_shouldUseBaselinePersona();
    void submit_whenMemoryDatabaseIsUnavailable_shouldReturnContextWithoutPersistentHints();
    void submit_whenRuntimeMetadataIsProvided_shouldReturnCompleteSnapshot();
    void submit_whenPromptAndBaselineChange_shouldUseLatestRequestSnapshot();
    void retrieve_whenNeighborOnlyMatchesThroughRelation_shouldExpandGraphWithoutReinforcement();
    void stop_whenPreparationIsDelayed_shouldReturnImmediatelyAndSuppressResult();
    void stop_whenRestartIsRequestedDuringShutdown_shouldDispatchPendingRequestOnce();
    void stop_whenPendingRestartIsStoppedAgain_shouldDiscardPendingRequest();
    void resources_whenExecutorIsDestroyed_shouldOpenLoadAndCloseOnWorkerThread();
    void retrieve_whenMemoriesMatch_shouldReturnRankedResultsWithoutPersistenceMutation();
};

void ChatPreparationExecutorTests::
start_whenEnvironmentIsValid_shouldCreateWorkerOwnedResources() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MemoryStore store;
    store.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
    store.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(store.load());
    ChatPreparationExecutor executor;

    const auto started = executor.start(environmentFor(directory));

    QVERIFY(started.isOk());
    ChatPreparationResult result;
    QVERIFY(prepareOnce(executor, requestFor(), &result));
    QVERIFY(result.error.code.isEmpty());
    QVERIFY(!result.messages.isEmpty());
    QCOMPARE(result.requestId, QStringLiteral("request-1"));
    QCOMPARE(result.generation, quint64(7));
}

void ChatPreparationExecutorTests::
start_whenDatabasePathsAreInvalid_shouldReturnControlledFailure() {
    ChatPreparationEnvironment environment;
    environment.profileId = QStringLiteral("profile-test");
    ChatPreparationExecutor executor;

    const auto started = executor.start(environment);

    QVERIFY(!started.isOk());
    QCOMPARE(started.error().code, QStringLiteral("CHAT_PREPARATION_INVALID_ENVIRONMENT"));
}

void ChatPreparationExecutorTests::
submit_whenContextIsValid_shouldReturnMessagesAndReinforcementIds() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MemoryStore store;
    store.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
    store.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(store.load());
    const MemoryEntry stored = store.addEntry(matchingMemory());
    QCOMPARE(stored.accessCount, 0);

    ChatPreparationExecutor executor;
    QVERIFY(executor.start(environmentFor(directory)).isOk());
    ChatPreparationRequest request = requestFor();
    ChatMessage prior;
    prior.role = QStringLiteral("assistant");
    prior.content = QStringLiteral("上一轮回答");
    request.conversationMemory.append(prior);
    WorkingMemoryItem working;
    working.id = QStringLiteral("working-1");
    working.summary = QStringLiteral("爵士乐播放列表");
    working.content = working.summary;
    working.createdAt = QDateTime::currentDateTimeUtc();
    request.workingMemory.append(working);

    ChatPreparationResult result;
    QVERIFY(prepareOnce(executor, std::move(request), &result));

    QVERIFY(result.error.code.isEmpty());
    QVERIFY(result.messages.size() >= 3);
    QCOMPARE(result.messages.first().role, QStringLiteral("system"));
    QCOMPARE(result.messages.at(1).content, QStringLiteral("上一轮回答"));
    QVERIFY(result.messages.last().content.contains(QStringLiteral("相关记忆")));
    QVERIFY(result.reinforcementIds.contains(QStringLiteral("memory-jazz")));
    const MemoryEntry* unchanged = store.findById(QStringLiteral("memory-jazz"));
    QVERIFY(unchanged);
    QCOMPARE(unchanged->accessCount, 0);
}

void ChatPreparationExecutorTests::
submit_whenIdentityDatabaseIsUnavailable_shouldUseBaselinePersona() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ChatPreparationEnvironment environment = environmentFor(directory);
    environment.runtimeDatabasePath = directory.path();
    ChatPreparationExecutor executor;
    QVERIFY(executor.start(environment).isOk());

    ChatPreparationRequest request = requestFor();
    request.runtimeMetadata.runtimeDatabasePath = directory.path();
    request.identityBaseline.speakingStyle = QStringLiteral("测试基线风格");
    ChatPreparationResult result;
    QVERIFY(prepareOnce(executor, std::move(request), &result));

    QVERIFY(result.error.code.isEmpty());
    QVERIFY(result.messages.first().content.contains(QStringLiteral("测试基线风格")));
}

void ChatPreparationExecutorTests::
submit_whenMemoryDatabaseIsUnavailable_shouldReturnContextWithoutPersistentHints() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ChatPreparationEnvironment environment = environmentFor(directory);
    environment.memoryDatabasePath = directory.path();
    ChatPreparationExecutor executor;
    QVERIFY(executor.start(environment).isOk());

    ChatPreparationResult result;
    QVERIFY(prepareOnce(executor, requestFor(), &result));

    QVERIFY(result.error.code.isEmpty());
    QVERIFY(!result.messages.isEmpty());
    QVERIFY(result.reinforcementIds.isEmpty());
    QVERIFY(!result.messages.last().content.contains(QStringLiteral("相关记忆")));
}

void ChatPreparationExecutorTests::
submit_whenRuntimeMetadataIsProvided_shouldReturnCompleteSnapshot() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString memoryDirectory = directory.filePath(QStringLiteral("legacy"));
    const QString runtimeDirectory = directory.filePath(QStringLiteral("profiles/runtime"));
    QVERIFY(QDir().mkpath(memoryDirectory));
    QVERIFY(QDir().mkpath(runtimeDirectory));
    const QString memoryPath = QDir(memoryDirectory).filePath(QStringLiteral("memory.db"));
    const QString runtimePath = QDir(runtimeDirectory).filePath(
        QStringLiteral("agent_runtime.sqlite"));
    const QString profileId = QStringLiteral("11111111-1111-4111-8111-111111111111");

    MemoryStore store;
    store.setDatabasePath(memoryPath);
    store.setStoragePath(QDir(memoryDirectory).filePath(QStringLiteral("memory.json")));
    QVERIFY(store.load());

    SqliteEventRepository schemaRepository;
    QVERIFY(schemaRepository.open(runtimePath).isOk());
    SqliteIdentityRepository identity;
    QVERIFY(identity.open(runtimePath).isOk());
    PersonalitySnapshot personality;
    personality.stateId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    personality.profileId = profileId;
    personality.version = 1;
    personality.effectiveAt = QDateTime::currentDateTimeUtc();
    personality.createdAt = personality.effectiveAt;
    SqliteRuntimeUnitOfWorkFactory unitOfWorkFactory(runtimePath);
    auto unitOfWork = unitOfWorkFactory.begin();
    QVERIFY(unitOfWork.isOk());
    QVERIFY(identity.appendPersonalityState(*unitOfWork.value(), personality, {}).isOk());
    QVERIFY(unitOfWork.value()->commit().isOk());

    RelationshipSnapshot relationship;
    relationship.stateId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    relationship.profileId = profileId;
    relationship.subjectId = QStringLiteral("owner");
    relationship.version = 1;
    relationship.effectiveAt = personality.effectiveAt;
    relationship.createdAt = personality.createdAt;
    QVERIFY(identity.appendRelationshipState(relationship).isOk());

    SelfModelSnapshot selfModel;
    selfModel.versionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    selfModel.profileId = profileId;
    selfModel.narrative = QStringLiteral("stable test self model");
    selfModel.effectiveAt = personality.effectiveAt;
    selfModel.createdAt = personality.createdAt;
    QVERIFY(identity.appendSelfModelState(selfModel).isOk());
    identity.close();

    ChatPreparationEnvironment environment = environmentFor(directory);
    environment.memoryDatabasePath = memoryPath;
    ChatPreparationExecutor executor;
    QVERIFY(executor.start(environment).isOk());
    ChatPreparationRequest request = requestFor();
    request.runtimeMetadata.profileId = profileId;
    request.runtimeMetadata.runtimeDatabasePath = runtimePath;
    request.runtimeMetadata.identityBaselineSchemaVersion = 7;
    request.runtimeMetadata.identityBaselineHash = QString(64, QLatin1Char('b'));
    request.runtimeMetadata.configHash = QString(64, QLatin1Char('c'));

    ChatPreparationResult result;
    QVERIFY(prepareOnce(executor, std::move(request), &result));

    QVERIFY(result.runtimeSnapshot.has_value());
    const RuntimeSnapshot& snapshot = *result.runtimeSnapshot;
    QCOMPARE(snapshot.profileId, profileId);
    QCOMPARE(snapshot.subjectId, QStringLiteral("owner"));
    QCOMPARE(snapshot.identityBaselineSchemaVersion, 7);
    QCOMPARE(snapshot.identityBaselineHash, QString(64, QLatin1Char('b')));
    QCOMPARE(snapshot.configHash, QString(64, QLatin1Char('c')));
    QCOMPARE(snapshot.personalityVersion, std::optional<qint64>(1));
    QCOMPARE(snapshot.relationshipVersion, std::optional<qint64>(1));
    QCOMPARE(snapshot.selfModelVersion, std::optional<QString>(selfModel.versionId));
}

void ChatPreparationExecutorTests::
submit_whenPromptAndBaselineChange_shouldUseLatestRequestSnapshot() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MemoryStore store;
    store.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
    store.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(store.load());
    ChatPreparationExecutor executor;
    QVERIFY(executor.start(environmentFor(directory)).isOk());

    ChatPreparationRequest first = requestFor();
    first.identityBaseline.speakingStyle = QStringLiteral("first-style");
    ChatPreparationResult firstResult;
    QVERIFY(prepareOnce(executor, std::move(first), &firstResult));
    QVERIFY(firstResult.messages.first().content.contains(QStringLiteral("first-style")));

    ChatPreparationRequest second = requestFor();
    second.requestId = QStringLiteral("request-2");
    second.identityBaseline.speakingStyle = QStringLiteral("second-style");
    second.promptTemplate.systemPromptBody = QStringLiteral(
        "dynamic-template {{speaking_style}}");
    ChatPreparationResult secondResult;
    QVERIFY(prepareOnce(executor, std::move(second), &secondResult));

    QVERIFY(secondResult.messages.first().content.contains(
        QStringLiteral("dynamic-template second-style")));
    QVERIFY(!secondResult.messages.first().content.contains(QStringLiteral("first-style")));
}

void ChatPreparationExecutorTests::
retrieve_whenNeighborOnlyMatchesThroughRelation_shouldExpandGraphWithoutReinforcement() {
    MemoryEntry source = matchingMemory();
    MemoryEntry neighbor = matchingMemory();
    neighbor.id = QStringLiteral("memory-neighbor");
    neighbor.type = MemoryType::Semantic;
    neighbor.summary = QStringLiteral("只通过关系图发现的邻居");
    neighbor.content = neighbor.summary;
    MemoryRelation relation;
    relation.id = QStringLiteral("relation-1");
    relation.fromMemoryId = source.id;
    relation.toMemoryId = neighbor.id;
    relation.weight = 1.0;
    MemoryQuery query;
    query.text = QStringLiteral("爵士乐");
    query.preferredTypes = {MemoryType::Preference};

    MemoryRetriever retriever;
    const auto retrieved = retriever.retrieve({source, neighbor}, query, {}, {relation});

    const auto expanded = std::find_if(
        retrieved.cbegin(), retrieved.cend(), [&neighbor](const RetrievedMemory& memory) {
            return memory.entry.id == neighbor.id;
        });
    QVERIFY(expanded != retrieved.cend());
    QVERIFY(expanded->fromGraphExpansion);
}

void ChatPreparationExecutorTests::
stop_whenPreparationIsDelayed_shouldReturnImmediatelyAndSuppressResult() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MemoryStore store;
    store.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
    store.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(store.load());
    const QString runtimePath = directory.filePath(
        QStringLiteral("agent_runtime.sqlite"));
    SqliteEventRepository schemaRepository;
    QVERIFY(schemaRepository.open(runtimePath).isOk());
    schemaRepository.close();
    ChatPreparationExecutor executor;
    std::atomic_bool delayStarted{false};
    std::atomic_bool memoryClosed{false};
    std::atomic_bool identityClosed{false};
    executor.setTestResourceLifecycleProbe(
        [&delayStarted, &memoryClosed, &identityClosed](const QString& phase, quintptr) {
            if (phase == QLatin1String("preparation.delay.started")) {
                delayStarted.store(true, std::memory_order_release);
            } else if (phase == QLatin1String("memory.close")) {
                memoryClosed.store(true, std::memory_order_release);
            } else if (phase == QLatin1String("identity.close")) {
                identityClosed.store(true, std::memory_order_release);
            }
        });
    QVERIFY(executor.start(environmentFor(directory)).isOk());
    ChatPreparationRequest warmup = requestFor();
    warmup.runtimeMetadata.runtimeDatabasePath = runtimePath;
    ChatPreparationResult warmupResult;
    QVERIFY(prepareOnce(executor, std::move(warmup), &warmupResult));
    QVERIFY(warmupResult.error.code.isEmpty());

    executor.setTestPreparationDelayMs(100);
    int resultCount = 0;
    connect(&executor, &ChatPreparationExecutor::prepared, this,
            [&resultCount](const ChatPreparationResult&) { ++resultCount; });
    ChatPreparationRequest delayed = requestFor();
    delayed.requestId = QStringLiteral("request-delayed");
    delayed.runtimeMetadata.runtimeDatabasePath = runtimePath;
    executor.submit(std::move(delayed));
    QTRY_VERIFY_WITH_TIMEOUT(delayStarted.load(std::memory_order_acquire), 200);

    QElapsedTimer timer;
    timer.start();
    executor.stop();

    QVERIFY2(timer.elapsed() < 20, "normal stop must not wait behind preparation");
    QTRY_VERIFY_WITH_TIMEOUT(memoryClosed.load(std::memory_order_acquire), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(identityClosed.load(std::memory_order_acquire), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(!executor.isWorkerThreadRunningForTests(), 1000);
    QCOMPARE(resultCount, 0);

    executor.setTestPreparationDelayMs(0);
    QVERIFY(executor.start(environmentFor(directory)).isOk());
    ChatPreparationRequest restarted = requestFor();
    restarted.requestId = QStringLiteral("request-restarted");
    restarted.runtimeMetadata.runtimeDatabasePath = runtimePath;
    ChatPreparationResult restartedResult;
    QVERIFY(prepareOnce(executor, std::move(restarted), &restartedResult));
    QVERIFY(restartedResult.error.code.isEmpty());
    QCOMPARE(restartedResult.requestId, QStringLiteral("request-restarted"));
}

void ChatPreparationExecutorTests::
stop_whenRestartIsRequestedDuringShutdown_shouldDispatchPendingRequestOnce() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MemoryStore store;
    store.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
    store.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(store.load());
    ChatPreparationExecutor executor;
    std::atomic_bool delayStarted{false};
    executor.setTestResourceLifecycleProbe(
        [&delayStarted](const QString& phase, quintptr) {
            if (phase == QLatin1String("preparation.delay.started")) {
                delayStarted.store(true, std::memory_order_release);
            }
        });
    QVERIFY(executor.start(environmentFor(directory)).isOk());
    executor.setTestPreparationDelayMs(100);
    QList<ChatPreparationResult> results;
    connect(&executor, &ChatPreparationExecutor::prepared, this,
            [&results](ChatPreparationResult result) {
                results.append(std::move(result));
            });
    ChatPreparationRequest stale = requestFor();
    stale.requestId = QStringLiteral("request-stale");
    executor.submit(std::move(stale));
    QTRY_VERIFY_WITH_TIMEOUT(delayStarted.load(std::memory_order_acquire), 200);

    executor.stop();
    const auto restarted = executor.start(environmentFor(directory));
    QVERIFY(restarted.isOk());
    executor.setTestPreparationDelayMs(0);
    ChatPreparationRequest pending = requestFor();
    pending.requestId = QStringLiteral("request-pending-restart");
    executor.submit(std::move(pending));

    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 2000);
    QCOMPARE(results.first().requestId,
             QStringLiteral("request-pending-restart"));
    QVERIFY(results.first().error.code.isEmpty());
    QTest::qWait(50);
    QCOMPARE(results.size(), 1);
}

void ChatPreparationExecutorTests::
stop_whenPendingRestartIsStoppedAgain_shouldDiscardPendingRequest() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MemoryStore store;
    store.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
    store.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(store.load());
    ChatPreparationExecutor executor;
    std::atomic_bool delayStarted{false};
    executor.setTestResourceLifecycleProbe(
        [&delayStarted](const QString& phase, quintptr) {
            if (phase == QLatin1String("preparation.delay.started")) {
                delayStarted.store(true, std::memory_order_release);
            }
        });
    QVERIFY(executor.start(environmentFor(directory)).isOk());
    executor.setTestPreparationDelayMs(100);
    int resultCount = 0;
    connect(&executor, &ChatPreparationExecutor::prepared, this,
            [&resultCount](const ChatPreparationResult&) { ++resultCount; });
    ChatPreparationRequest stale = requestFor();
    stale.requestId = QStringLiteral("request-stale-before-second-stop");
    executor.submit(std::move(stale));
    QTRY_VERIFY_WITH_TIMEOUT(delayStarted.load(std::memory_order_acquire), 200);

    executor.stop();
    QVERIFY(executor.start(environmentFor(directory)).isOk());
    ChatPreparationRequest pending = requestFor();
    pending.requestId = QStringLiteral("request-discard-on-second-stop");
    executor.submit(std::move(pending));
    executor.stop();

    QTRY_VERIFY_WITH_TIMEOUT(!executor.isWorkerThreadRunningForTests(), 1000);
    QTest::qWait(50);
    QCOMPARE(resultCount, 0);
}

void ChatPreparationExecutorTests::
resources_whenExecutorIsDestroyed_shouldOpenLoadAndCloseOnWorkerThread() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MemoryStore store;
    store.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
    store.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(store.load());
    QList<QPair<QString, quintptr>> lifecycle;
    const quintptr testThreadId = reinterpret_cast<quintptr>(QThread::currentThreadId());
    {
        ChatPreparationExecutor executor;
        executor.setTestResourceLifecycleProbe(
            [&lifecycle](const QString& phase, quintptr threadId) {
                lifecycle.append({phase, threadId});
            });
        QVERIFY(executor.start(environmentFor(directory)).isOk());
        ChatPreparationResult result;
        QVERIFY(prepareOnce(executor, requestFor(), &result));
    }

    const QStringList requiredPhases = {
        QStringLiteral("memory.open"),
        QStringLiteral("memory.load"),
        QStringLiteral("memory.close")
    };
    for (const QString& phase : requiredPhases) {
        const auto observed = std::find_if(
            lifecycle.cbegin(), lifecycle.cend(), [&phase](const auto& item) {
                return item.first == phase;
            });
        QVERIFY2(observed != lifecycle.cend(), qPrintable(phase));
        QCOMPARE(observed->second, lifecycle.first().second);
        QVERIFY(observed->second != testThreadId);
    }
}

void ChatPreparationExecutorTests::
retrieve_whenMemoriesMatch_shouldReturnRankedResultsWithoutPersistenceMutation() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MemoryStore store;
    store.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
    store.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
    QVERIFY(store.load());
    store.addEntry(matchingMemory());
    MemoryQuery query;
    query.text = QStringLiteral("爵士乐");
    query.preferredTypes = {MemoryType::Preference};

    MemoryRetriever retriever;
    const auto retrieved = retriever.retrieve(store.all(), query, {});

    QCOMPARE(retrieved.size(), 1);
    QCOMPARE(retrieved.first().entry.id, QStringLiteral("memory-jazz"));
    const MemoryEntry* unchanged = store.findById(QStringLiteral("memory-jazz"));
    QVERIFY(unchanged);
    QCOMPARE(unchanged->accessCount, 0);
    QVERIFY(!unchanged->lastAccessedAt.isValid());
}

QTEST_MAIN(ChatPreparationExecutorTests)
#include "test_chat_preparation_executor.moc"
