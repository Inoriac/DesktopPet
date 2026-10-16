#include <QtTest>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QTimer>
#include <QSqlQuery>
#include <QSqlDatabase>
#include <QMutex>
#include <QMutexLocker>
#include <limits>
#include <stdexcept>
#include "ai/memory/hnsw_embedding_index.h"
#include "ai/memory/memory_index_worker.h"
#ifdef DESKTOP_PET_HAS_ORT
#include "ai/memory/onnx_embedding_provider.h"
#endif

#include <optional>
#include <atomic>

#include "ai/chat/chat_preparation_executor.h"
#include "ai/event/runtime_unit_of_work.h"
#include "ai/event/sqlite_event_repository.h"
#include "ai/identity/sqlite_identity_repository.h"
#include "ai/memory/memory_relation.h"
#include "ai/memory/memory_retriever.h"
#include "ai/memory/memory_store.h"
#include "ai/memory/active_memory_pool.h"
#include "ai/memory/hippocampus_working_set.h"
#include "ai/memory/memory_keyword_index.h"

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
    request.runtimeMetadata.originKind = OriginKind::LocalOwner;
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
    QSignalSpy feedback(&executor, &ChatPreparationExecutor::recallFeedbackProcessed);
    const auto connection = QObject::connect(&executor, &ChatPreparationExecutor::prepared, &executor,
                     [&received, output, &executor](ChatPreparationResult result) {
                         executor.acceptRecallFeedback(result);
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
    if (received && output->recallFeedbackToken != 0 && feedback.isEmpty()) feedback.wait(2000);
    QObject::disconnect(connection);
    return received;
}

class WorkerTestProvider final : public EmbeddingProvider {
public:
    explicit WorkerTestProvider(std::shared_ptr<std::atomic_bool> wrongThread = {},
                                quintptr guiThread = 0)
        : m_wrongThread(std::move(wrongThread)), m_guiThread(guiThread) {}
    ~WorkerTestProvider() override { checkThread(); }
    QString modelName() const override { return QStringLiteral("worker-recall-test"); }
    int dimension() const override { return 3; }
    QVector<float> embed(const QString&) override { checkThread(); return {1.0f, 0.0f, 0.0f}; }
private:
    void checkThread() const {
        if (m_wrongThread && reinterpret_cast<quintptr>(QThread::currentThreadId()) == m_guiThread)
            m_wrongThread->store(true);
    }
    std::shared_ptr<std::atomic_bool> m_wrongThread;
    quintptr m_guiThread;
};

struct TopicProviderState {
    std::atomic<int> calls{0};
    std::atomic<int> failure{0};
    std::atomic<int> model{1};
    QMutex mutex;
    QStringList queryTexts;
};
class TopicTestProvider final : public EmbeddingProvider {
public:
    explicit TopicTestProvider(std::shared_ptr<TopicProviderState> state) : m_state(std::move(state)) {}
    QString modelName() const override { return QStringLiteral("topic-test-%1").arg(m_state->model.load()); }
    int dimension() const override { return 3; }
    QVector<float> embed(const QString& text) override {
        if (!text.contains(QLatin1Char('\n'))) {
            ++m_state->calls;
            QMutexLocker locker(&m_state->mutex);
            m_state->queryTexts.append(text);
            switch (m_state->failure.load()) {
            case 1: return {};
            case 2: return {std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f};
            case 3: return {1.f, 0.f};
            case 4: throw std::runtime_error("simulated encoder failure");
            default: break;
            }
        }
        if (text.contains(QStringLiteral("医院")) || text.contains(QStringLiteral("天气"))) return {0.f, 0.f, 1.f};
        if (text.contains(QStringLiteral("工作")) || text.contains(QStringLiteral("职业")) || text.contains(QStringLiteral("酒店"))) return {0.f, 1.f, 0.f};
        return {1.f, 0.f, 0.f};
    }
private:
    std::shared_ptr<TopicProviderState> m_state;
};

bool seedTopicVector(MemoryStore& store, const MemoryEntry& entry,
                     TopicTestProvider& provider, const QString& directory) {
    const QString text = entry.summary + QLatin1Char('\n') + entry.content;
    const auto vector = provider.embed(text);
    const auto hash = HnswEmbeddingIndex::contentHash(text);
    QSqlQuery row(QSqlDatabase::database(store.databaseConnectionName(), false));
    row.prepare(QStringLiteral("INSERT OR REPLACE INTO memory_embeddings(memory_id,model,dimension,vector_blob,content_hash) VALUES(?,?,?,?,?)"));
    row.addBindValue(entry.id);
    row.addBindValue(provider.modelName());
    row.addBindValue(vector.size());
    row.addBindValue(QByteArray(reinterpret_cast<const char*>(vector.constData()), vector.size()*sizeof(float)));
    row.addBindValue(hash);
    if (!row.exec()) return false;
    HnswEmbeddingIndex index(store.databaseConnectionName(), &provider, directory);
    return index.rebuildFromRepository() && index.saveToDisk();
}

bool fillRecentWindow(MemoryStore& store) {
    for (int i = 0; i < 300; ++i) {
        MemoryEntry recent;
        recent.type = MemoryType::ShortTerm;
        recent.id = QStringLiteral("recent-%1").arg(i);
        recent.key = recent.id;
        recent.summary = QStringLiteral("recent unrelated event %1").arg(i);
        recent.createdAt = QDateTime::currentDateTimeUtc();
        recent.updatedAt = recent.createdAt;
        if (store.addEntry(recent).id.isEmpty()) return false;
    }
    return true;
}

} // namespace

class ChatPreparationExecutorTests : public QObject {
    Q_OBJECT

private slots:
    void prepare_externalContextFiltersWorkingMemoryBeforeQuery();
    void chatPreparation_emptySubject_respectsOrigin_data();
    void chatPreparation_emptySubject_respectsOrigin();

    void avatarDescriptionAndStateRemainSeparateFromIdentity();
    void workingMemory_shouldReachPromptWithoutPersistenceAndFilterExpiredOrSensitiveItems_data();
    void workingMemory_shouldReachPromptWithoutPersistenceAndFilterExpiredOrSensitiveItems();
    void proactiveRecall_shouldUseRecentTopicAndEmotionAlongsidePersona();
    void personality_shouldChangeAssociationStrengthInWorkerRecall();
    void wholeTagRecallFindsOldMemoryWithoutEmbeddings();
    void semanticRecallFindsOldMemoryAndRejectsStalePrivateHits();
    void activePoolSurvivesWorkerRestartAndClear();
    void topicRecallReusesCurrentCandidatesWithoutAccumulatingActivation();
    void topicRecallSharesOneEmbeddingAcrossSemanticRewordings();
    void topicRecallChecksNewInputsWithoutHistoryContamination();
    void topicRecallRefreshesInboxButNotPostingsForMetadata();
    void topicRecallRejectsChangedContentAndEligibility_data();
    void topicRecallRejectsChangedContentAndEligibility();
    void topicRecallInvalidatesForGraphAndRawSqlChanges();
    void topicRecallRejectsFailedEmbeddings_data();
    void topicRecallRejectsFailedEmbeddings();
    void topicRecallOnlyLearnsConsumedResults();
    void topicRecallReusesCandidatesFromMoreThanOnePreviousTurn();
    void topicRecallAcceptedFeedbackDrainsBeforeStop();
    void topicRecallAutomaticTriggerEncodesContextAndPreservesUserTopic();
    void topicPoolRetainsMultipleTurnsAndExpiresDerivedState();
    void topicPoolGuardsChanges_data();
    void topicPoolGuardsChanges();
    void emotionBonusIsNotPersistedIntoActivationPool();
    void slowProviderInitializationDoesNotBlockGui();
#ifdef DESKTOP_PET_HAS_ORT
    void nativeOnnxRecallRunsThroughChatWorker();
#endif
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

void ChatPreparationExecutorTests::chatPreparation_emptySubject_respectsOrigin_data() {
    QTest::addColumn<int>("origin"); QTest::addColumn<bool>("owner");
    QTest::newRow("external") << int(OriginKind::External) << false;
    QTest::newRow("internal") << int(OriginKind::Internal) << false;
    QTest::newRow("local") << int(OriginKind::LocalOwner) << true;
}
void ChatPreparationExecutorTests::chatPreparation_emptySubject_respectsOrigin() {
    QFETCH(int, origin); QFETCH(bool, owner);
    QTemporaryDir directory; const auto environment = environmentFor(directory);
    SqliteEventRepository schema; QVERIFY(schema.open(environment.runtimeDatabasePath).isOk());
    SqliteIdentityRepository identities; QVERIFY(identities.open(environment.runtimeDatabasePath).isOk());
    RelationshipSnapshot relationship;
    relationship.stateId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    relationship.profileId = QStringLiteral("profile-test"); relationship.subjectId = QStringLiteral("owner");
    relationship.version = 1; relationship.createdAt = QDateTime::currentDateTimeUtc();
    relationship.effectiveAt = relationship.createdAt;
    QVERIFY(identities.appendRelationshipState(relationship).isOk());
    ChatPreparationExecutor executor; QVERIFY(executor.start(environment).isOk());
    auto request = requestFor(); request.runtimeMetadata.subjectId.clear();
    request.runtimeMetadata.originKind = static_cast<OriginKind>(origin);
    request.runtimeMetadata.runtimeDatabasePath = environment.runtimeDatabasePath;
    ChatPreparationResult result; QVERIFY(prepareOnce(executor, std::move(request), &result));
    QVERIFY(result.runtimeSnapshot.has_value());
    QCOMPARE(result.runtimeSnapshot->subjectId, owner ? QStringLiteral("owner") : QString());
    QCOMPARE(result.runtimeSnapshot->relationshipVersion, owner ? std::optional<qint64>(1) : std::nullopt);
}

void ChatPreparationExecutorTests::workingMemory_shouldReachPromptWithoutPersistenceAndFilterExpiredOrSensitiveItems_data() {
    QTest::addColumn<bool>("persistentStore");
    QTest::newRow("persistent-store") << true;
    QTest::newRow("temporary-only") << false;
}

void ChatPreparationExecutorTests::workingMemory_shouldReachPromptWithoutPersistenceAndFilterExpiredOrSensitiveItems() {
    QFETCH(bool, persistentStore);
    QTemporaryDir directory;
    MemoryStore store;
    auto environment = environmentFor(directory);
    if (persistentStore) {
        store.setDatabasePath(environment.memoryDatabasePath);
        QVERIFY(store.loadDatabaseOnly());
    } else {
        // A directory cannot be opened as a SQLite database.
        environment.memoryDatabasePath = directory.path();
    }
    ChatPreparationExecutor executor;
    QVERIFY(executor.start(environment).isOk());
    auto request = requestFor(QStringLiteral("刚才查询的结果是什么？"));
    WorkingMemoryItem item;
    item.id = QStringLiteral("tool-weather");
    item.source = QStringLiteral("tool_result");
    item.summary = QStringLiteral("天气查询成功");
    item.content = QStringLiteral("天气查询成功：杭州，晴，23摄氏度");
    item.createdAt = QDateTime::currentDateTimeUtc();
    item.expiresAt = item.createdAt.addSecs(900);
    request.workingMemory.append(item);
    item.id = QStringLiteral("expired");
    item.content = item.summary = QStringLiteral("不应看到的过期结果");
    item.expiresAt = item.createdAt.addSecs(-1);
    request.workingMemory.append(item);
    item.id = QStringLiteral("private");
    item.content = item.summary = QStringLiteral("不应看到的敏感结果");
    item.expiresAt = item.createdAt.addSecs(900);
    item.privacyLevel = PrivacyLevel::Sensitive;
    request.workingMemory.append(item);
    ChatPreparationResult result;
    QVERIFY(prepareOnce(executor, request, &result));
    const auto context = result.messages.last().content;
    QVERIFY(context.contains(QStringLiteral("23摄氏度")));
    QVERIFY(!context.contains(QStringLiteral("不应看到")));
    QVERIFY(result.reinforcementIds.isEmpty());
    if (persistentStore) QVERIFY(store.loadActiveMemorySnapshot().isEmpty());
    QVERIFY(store.all().isEmpty());
}

void ChatPreparationExecutorTests::proactiveRecall_shouldUseRecentTopicAndEmotionAlongsidePersona() {
    QTemporaryDir directory;
    auto environment = environmentFor(directory);
    MemoryStore store;
    store.setDatabasePath(environment.memoryDatabasePath);
    QVERIFY(store.loadDatabaseOnly());
    auto memory = matchingMemory();
    memory.tags = {QStringLiteral("爵士乐")};
    store.addEntry(memory);
    SqliteEventRepository schema;
    QVERIFY(schema.open(environment.runtimeDatabasePath).isOk());
    schema.close();
    SqliteIdentityRepository identity;
    QVERIFY(identity.open(environment.runtimeDatabasePath).isOk());
    ChatPreparationExecutor executor;
    QVERIFY(executor.start(environment).isOk());
    auto request = requestFor(QStringLiteral("proactive_chat_tick"));
    request.triggerTag = QStringLiteral("proactive_chat");
    request.runtimeMetadata.runtimeDatabasePath = environment.runtimeDatabasePath;
    ChatMessage prior;
    prior.role = QStringLiteral("user");
    prior.content = QStringLiteral("最近在练习爵士乐");
    request.conversationMemory = {prior};
    EmotionSnapshot mood;
    mood.active = EmotionType::Joy;
    mood.moodValence = 0.8;
    mood.intensity = 0.7;
    mood.updatedAt = QDateTime::currentDateTimeUtc();
    request.emotion = mood;
    ChatPreparationResult result;
    QVERIFY(prepareOnce(executor, request, &result));
    QVERIFY(result.runtimeSnapshot.has_value());
    QVERIFY(result.messages.last().content.contains(QStringLiteral("主人喜欢爵士乐")));
    QVERIFY(result.messages.last().content.contains(QStringLiteral("active_emotion=joy")));
    QVERIFY(result.reinforcementIds.contains(memory.id));
}

void ChatPreparationExecutorTests::personality_shouldChangeAssociationStrengthInWorkerRecall() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(environmentFor(directory).memoryDatabasePath);
    QVERIFY(store.loadDatabaseOnly());
    const auto source = store.addEntry(matchingMemory());
    MemoryEntry neighbor = matchingMemory();
    neighbor.id = QStringLiteral("association-neighbor");
    neighbor.type = MemoryType::Semantic;
    neighbor.summary = neighbor.content = QStringLiteral("海边黄昏的经历");
    neighbor = store.addEntry(neighbor);
    MemoryRelation relation;
    relation.id = QStringLiteral("jazz-seaside");
    relation.fromMemoryId = source.id;
    relation.toMemoryId = neighbor.id;
    relation.type = MemoryRelationType::Related;
    QVERIFY(store.relationGraph().addRelation(relation));
    ActiveMemoryPool pool;
    HippocampusWorkingSet workingSet(&store);
    workingSet.refresh();
    MemoryKeywordIndex index;
    index.rebuild(store.all());
    MemoryQuery query;
    query.text = QStringLiteral("爵士乐");
    query.personality = {{QStringLiteral("openness"), 0.0},
                         {QStringLiteral("sociability"), 0.0},
                         {QStringLiteral("initiative"), 0.35}};
    const auto quiet = retrieveWithGraphPropagationForWorker(
        store, pool, workingSet, index, nullptr, query);
    query.personality[QStringLiteral("sociability")] = 1.0;
    const auto outgoing = retrieveWithGraphPropagationForWorker(
        store, pool, workingSet, index, nullptr, query);
    const auto score = [&neighbor](const WorkerRecallResult& result) {
        for (const auto& memory : result.memories) {
            if (memory.entry.id == neighbor.id) return memory.score;
        }
        return -1.0;
    };
    QVERIFY(score(quiet) >= 0.0);
    QVERIFY(score(outgoing) > score(quiet));
}

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
    QVERIFY(result.messages.first().content.contains(selfModel.narrative));
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

    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
    QVERIFY(store.loadDatabaseOnly());
    QVERIFY(!store.addEntry(source).id.isEmpty());
    QVERIFY(!store.addEntry(neighbor).id.isEmpty());
    store.relationGraph().addRelation(relation);
    const auto retrieved = retrieveWithGraphPropagationForWorker(
        store.databasePath(), query).memories;

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

    const auto retrieved = retrieveWithGraphPropagationForWorker(
        store.databasePath(), query).memories;

    QCOMPARE(retrieved.size(), 1);
    QCOMPARE(retrieved.first().entry.id, QStringLiteral("memory-jazz"));
    const MemoryEntry* unchanged = store.findById(QStringLiteral("memory-jazz"));
    QVERIFY(unchanged);
    QCOMPARE(unchanged->accessCount, 0);
    QVERIFY(!unchanged->lastAccessedAt.isValid());
}

void ChatPreparationExecutorTests::topicRecallSharesOneEmbeddingAcrossSemanticRewordings() {
    QTemporaryDir directory;
    auto environment = environmentFor(directory);
    MemoryStore store;
    store.setDatabasePath(environment.memoryDatabasePath);
    QVERIFY(store.loadDatabaseOnly());
    auto entry = store.addEntry(matchingMemory());
    auto state = std::make_shared<TopicProviderState>();
    TopicTestProvider seedProvider(state);
    QVERIFY(seedTopicVector(store, entry, seedProvider, directory.path()));
    environment.embeddingProviderFactory = [state] { return std::make_unique<TopicTestProvider>(state); };
    ChatPreparationExecutor executor;
    QVERIFY(executor.start(environment).isOk());
    ChatPreparationResult result;
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("爵士乐演出")), &result));
    QCOMPARE(result.recallDiagnostics.mode, QStringLiteral("full"));
    QCOMPARE(result.recallDiagnostics.queryEmbeddingCalls, 1);
    QCOMPARE(result.recallDiagnostics.indexQueries, 1);
    QVERIFY(result.recallDiagnostics.graphExpanded);
    QCOMPARE(state->calls.load(), 1);
    QVERIFY(result.reinforcementIds.contains(entry.id));
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("想听点即兴演奏")), &result));
    QCOMPARE(result.recallDiagnostics.mode, QStringLiteral("reuse"));
    QCOMPARE(result.recallDiagnostics.queryEmbeddingCalls, 1);
    QCOMPARE(result.recallDiagnostics.indexQueries, 0);
    QVERIFY(!result.recallDiagnostics.graphExpanded);
    QCOMPARE(state->calls.load(), 2);
    QVERIFY(result.reinforcementIds.contains(entry.id));
    QVERIFY(!result.recallDiagnostics.cacheRebuilt);
    QCOMPARE(store.readForRecall(entry.id)->accessCount, 0);
    QCOMPARE(store.readForRecall(entry.id)->mentionCount, entry.mentionCount);
    // A model switch cannot reuse vectors or topic evidence from its predecessor.
    state->model.store(2);
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("想听点即兴演奏")), &result));
    QCOMPARE(result.recallDiagnostics.mode, QStringLiteral("full"));
    QCOMPARE(result.recallDiagnostics.reason, QStringLiteral("embedding_changed"));
    QCOMPARE(state->calls.load(), 3);
}

void ChatPreparationExecutorTests::topicRecallChecksNewInputsWithoutHistoryContamination() {
    QTemporaryDir directory;
    auto environment = environmentFor(directory);
    MemoryStore store;
    store.setDatabasePath(environment.memoryDatabasePath);
    QVERIFY(store.loadDatabaseOnly());
    auto entry = matchingMemory();
    entry.summary = entry.content = QStringLiteral("杭州景点旅游计划");
    entry = store.addEntry(entry);
    auto state = std::make_shared<TopicProviderState>();
    TopicTestProvider provider(state);
    QVERIFY(seedTopicVector(store, entry, provider, directory.path()));
    environment.embeddingProviderFactory = [state] { return std::make_unique<TopicTestProvider>(state); };
    ChatPreparationExecutor executor;
    QVERIFY(executor.start(environment).isOk());
    ChatPreparationResult result;
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("杭州景点")), &result));
    auto next = requestFor(QStringLiteral("杭州医院"));
    next.conversationMemory.append({QStringLiteral("user"), QStringLiteral("杭州景点旅游计划")});
    QVERIFY(prepareOnce(executor, next, &result));
    QCOMPARE(result.recallDiagnostics.mode, QStringLiteral("full"));
    QCOMPARE(result.recallDiagnostics.indexQueries, 1);
    {
        QMutexLocker locker(&state->mutex);
        QCOMPARE(state->queryTexts.last(), QStringLiteral("杭州医院"));
    }
    next.reason = QStringLiteral("上海天气");
    QVERIFY(prepareOnce(executor, next, &result));
    QCOMPARE(result.recallDiagnostics.mode, QStringLiteral("full"));
    QMutexLocker locker(&state->mutex);
    QCOMPARE(state->queryTexts.last(), QStringLiteral("上海天气"));
}

void ChatPreparationExecutorTests::topicRecallRefreshesInboxButNotPostingsForMetadata() {
    QTemporaryDir directory;
    auto environment = environmentFor(directory);
    MemoryStore store;
    store.setDatabasePath(environment.memoryDatabasePath);
    QVERIFY(store.loadDatabaseOnly());
    auto entry = store.addEntry(matchingMemory());
    ChatPreparationExecutor executor;
    QVERIFY(executor.start(environment).isOk());
    ChatPreparationResult result;
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("爵士乐")), &result));
    const auto revisions = store.recallRevisions();
    store.reinforceEntries({entry.id});
    auto latest = *store.readForRecall(entry.id);
    latest.lastMentionedAt = QDateTime::currentDateTimeUtc();
    ++latest.mentionCount;
    latest.payload.insert(QStringLiteral("request_id"), QStringLiteral("new-write"));
    QVERIFY(store.updateEntryById(latest));
    QCOMPARE(store.recallRevisions(), revisions);
    auto inbox = matchingMemory();
    inbox.id = QStringLiteral("new-inbox-condition");
    inbox.type = MemoryType::ShortTerm;
    inbox.partition = QStringLiteral("hippocampus");
    inbox.summary = inbox.content = QStringLiteral("爵士乐最新场地就在家附近");
    inbox = store.addEntry(inbox);
    QVERIFY(!inbox.id.isEmpty());
    QCOMPARE(store.recallRevisions(), revisions);
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("爵士乐")), &result));
    QCOMPARE(result.recallDiagnostics.mode, QStringLiteral("reuse"));
    QVERIFY(!result.recallDiagnostics.cacheRebuilt);
    QVERIFY(result.reinforcementIds.contains(inbox.id));
    QCOMPARE(store.readForRecall(entry.id)->mentionCount, latest.mentionCount);
}

void ChatPreparationExecutorTests::topicRecallRejectsChangedContentAndEligibility_data() {
    QTest::addColumn<QString>("mutation");
    QTest::newRow("content") << QStringLiteral("UPDATE memory_items SET summary='unrelated bookkeeping',content='unrelated bookkeeping' WHERE id='memory-jazz'");
    QTest::newRow("private") << QStringLiteral("UPDATE memory_items SET privacy_level='sensitive' WHERE id='memory-jazz'");
    QTest::newRow("expired") << QStringLiteral("UPDATE memory_items SET expires_at='2000-01-01T00:00:00Z' WHERE id='memory-jazz'");
    QTest::newRow("deleted") << QStringLiteral("DELETE FROM memory_items WHERE id='memory-jazz'");
    QTest::newRow("archived") << QStringLiteral("UPDATE memory_items SET status='archived' WHERE id='memory-jazz'");
}
void ChatPreparationExecutorTests::topicRecallRejectsChangedContentAndEligibility() {
    QFETCH(QString, mutation);
    QTemporaryDir directory;
    auto environment = environmentFor(directory);
    MemoryStore store;
    store.setDatabasePath(environment.memoryDatabasePath);
    QVERIFY(store.loadDatabaseOnly());
    const auto entry = store.addEntry(matchingMemory());
    auto state = std::make_shared<TopicProviderState>();
    TopicTestProvider provider(state);
    QVERIFY(seedTopicVector(store, entry, provider, directory.path()));
    environment.embeddingProviderFactory = [state] { return std::make_unique<TopicTestProvider>(state); };
    ChatPreparationExecutor executor;
    QVERIFY(executor.start(environment).isOk());
    ChatPreparationResult result;
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("想听点即兴演奏")), &result));
    QVERIFY(result.reinforcementIds.contains(entry.id));
    QSqlQuery change(QSqlDatabase::database(store.databaseConnectionName(), false));
    QVERIFY(change.exec(mutation));
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("想听点即兴演奏")), &result));
    QCOMPARE(result.recallDiagnostics.mode, QStringLiteral("full"));
    QVERIFY(result.recallDiagnostics.cacheRebuilt);
    QVERIFY(!result.reinforcementIds.contains(entry.id));
    QVERIFY(!result.messages.last().content.contains(QStringLiteral("unrelated bookkeeping")));
}

void ChatPreparationExecutorTests::topicRecallInvalidatesForGraphAndRawSqlChanges() {
    QTemporaryDir directory;
    auto environment = environmentFor(directory);
    MemoryStore store;
    store.setDatabasePath(environment.memoryDatabasePath);
    QVERIFY(store.loadDatabaseOnly());
    auto entry = store.addEntry(matchingMemory());
    auto neighbor = matchingMemory();
    neighbor.id = QStringLiteral("other");
    neighbor.summary = neighbor.content = QStringLiteral("独立关联线索");
    QVERIFY(!store.addEntry(neighbor).id.isEmpty());
    ChatPreparationExecutor executor;
    QVERIFY(executor.start(environment).isOk());
    ChatPreparationResult result;
    QVERIFY(prepareOnce(executor, requestFor(), &result));
    QSqlQuery sql(QSqlDatabase::database(store.databaseConnectionName(), false));
    QVERIFY(!result.messages.last().content.contains(neighbor.summary));
    const auto before = store.recallRevisions();
    QVERIFY(sql.exec(QStringLiteral("UPDATE memory_items SET scope='new_scope' WHERE id='memory-jazz'")));
    QVERIFY(store.recallRevisions().first > before.first);
    QVERIFY(prepareOnce(executor, requestFor(), &result));
    QVERIFY(result.recallDiagnostics.cacheRebuilt);
    QCOMPARE(result.recallDiagnostics.mode, QStringLiteral("full"));
    const auto scopeRevision = store.recallRevisions();
    QVERIFY(sql.exec(QStringLiteral("INSERT INTO memory_relations(id,from_memory_id,to_memory_id,relation_type,weight,confidence) VALUES('new-edge','memory-jazz','other','related',1,1)")));
    QVERIFY(store.recallRevisions().first > scopeRevision.first);
    QVERIFY(prepareOnce(executor, requestFor(), &result));
    QCOMPARE(result.recallDiagnostics.mode, QStringLiteral("full"));
    QVERIFY(result.recallDiagnostics.graphExpanded);
    QVERIFY(result.messages.last().content.contains(neighbor.summary));
    QVERIFY(!result.reinforcementIds.contains(neighbor.id));
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("忘记爵士乐")), &result));
    QVERIFY(!result.messages.last().content.contains(neighbor.summary));
    QVERIFY(!result.reinforcementIds.contains(entry.id));
    const auto graphRevision = store.recallRevisions();
    QVERIFY(sql.exec(QStringLiteral("UPDATE memory_items SET payload_json='{\"source_tags\":[\"爵士乐\"]}' WHERE id='memory-jazz'")));
    QVERIFY(store.recallRevisions().first > graphRevision.first);
    const auto contentRevision = store.recallRevisions();
    QVERIFY(sql.exec(QStringLiteral("UPDATE memory_items SET payload_json='{\"source_tags\":[\"爵士乐\"],\"request_id\":\"changed\"}' WHERE id='memory-jazz'")));
    QCOMPARE(store.recallRevisions(), contentRevision);
}

void ChatPreparationExecutorTests::topicRecallRejectsFailedEmbeddings_data() {
    QTest::addColumn<int>("failure");
    QTest::newRow("empty") << 1;
    QTest::newRow("nan") << 2;
    QTest::newRow("dimension") << 3;
    QTest::newRow("exception") << 4;
}
void ChatPreparationExecutorTests::topicRecallRejectsFailedEmbeddings() {
    QFETCH(int, failure);
    QTemporaryDir directory;
    auto environment = environmentFor(directory);
    MemoryStore store;
    store.setDatabasePath(environment.memoryDatabasePath);
    QVERIFY(store.loadDatabaseOnly());
    auto entry = store.addEntry(matchingMemory());
    auto state = std::make_shared<TopicProviderState>();
    TopicTestProvider provider(state);
    QVERIFY(seedTopicVector(store, entry, provider, directory.path()));
    environment.embeddingProviderFactory = [state] { return std::make_unique<TopicTestProvider>(state); };
    ChatPreparationExecutor executor;
    QVERIFY(executor.start(environment).isOk());
    ChatPreparationResult result;
    QVERIFY(prepareOnce(executor, requestFor(), &result));
    state->failure.store(failure);
    QVERIFY(prepareOnce(executor, requestFor(), &result));
    QCOMPARE(result.recallDiagnostics.mode, QStringLiteral("full"));
    QCOMPARE(result.recallDiagnostics.queryEmbeddingCalls, 1);
    QCOMPARE(result.recallDiagnostics.indexQueries, 0);
    QCOMPARE(state->calls.load(), 2);
    QVERIFY(result.reinforcementIds.contains(entry.id)); // lexical fallback remains useful
}

void ChatPreparationExecutorTests::topicRecallOnlyLearnsConsumedResults() {
    QTemporaryDir directory;
    auto environment = environmentFor(directory);
    MemoryStore store;
    store.setDatabasePath(environment.memoryDatabasePath);
    QVERIFY(store.loadDatabaseOnly());
    auto entry = store.addEntry(matchingMemory());
    ChatPreparationExecutor executor;
    QVERIFY(executor.start(environment).isOk());
    QSignalSpy prepared(&executor, &ChatPreparationExecutor::prepared);
    executor.submit(requestFor(QStringLiteral("爵士乐")));
    QVERIFY(prepared.wait(2000));
    auto abandoned = qvariant_cast<ChatPreparationResult>(prepared.first().first());
    QVERIFY(abandoned.reinforcementIds.contains(entry.id));
    QVERIFY(store.loadActiveMemorySnapshot().isEmpty());
    ChatPreparationResult consumed;
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("爵士乐")), &consumed));
    QCOMPARE(consumed.recallDiagnostics.mode, QStringLiteral("full"));
    QVERIFY(!store.loadActiveMemorySnapshot().isEmpty());
    QSignalSpy feedback(&executor, &ChatPreparationExecutor::recallFeedbackProcessed);
    executor.acceptRecallFeedback(abandoned);
    QVERIFY(feedback.wait(2000));
    QCOMPARE(feedback.last().at(1).toBool(), false);
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("爵士乐")), &consumed));
    QCOMPARE(consumed.recallDiagnostics.mode, QStringLiteral("reuse"));
    executor.stop();
    QVERIFY(executor.start(environment).isOk());
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("爵士乐")), &consumed));
    QCOMPARE(consumed.recallDiagnostics.mode, QStringLiteral("full")); // derived topic is not persisted
}

void ChatPreparationExecutorTests::topicRecallReusesCandidatesFromMoreThanOnePreviousTurn() {
    QTemporaryDir directory;
    auto environment = environmentFor(directory);
    MemoryStore store;
    store.setDatabasePath(environment.memoryDatabasePath);
    QVERIFY(store.loadDatabaseOnly());
    auto state = std::make_shared<TopicProviderState>();
    TopicTestProvider provider(state);
    auto sightseeing = matchingMemory();
    sightseeing.summary = sightseeing.content = QStringLiteral("杭州 景点");
    sightseeing.importance = sightseeing.strength = 1.0;
    sightseeing = store.addEntry(sightseeing);
    QVERIFY(seedTopicVector(store, sightseeing, provider, directory.path()));
    for (int i = 0; i < 8; ++i) {
        auto hotel = matchingMemory();
        hotel.id = QStringLiteral("hotel-%1").arg(i);
        hotel.summary = hotel.content = QStringLiteral("杭州 酒店 偏好%1").arg(QChar('a'+i));
        hotel.importance = hotel.strength = 1.0;
        hotel = store.addEntry(hotel);
        QVERIFY(seedTopicVector(store, hotel, provider, directory.path()));
    }
    environment.embeddingProviderFactory = [state] { return std::make_unique<TopicTestProvider>(state); };
    ChatPreparationExecutor executor;
    QVERIFY(executor.start(environment).isOk());
    ChatPreparationResult result;
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("杭州 景点")), &result));
    QVERIFY(result.reinforcementIds.contains(sightseeing.id));
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("杭州 酒店")), &result));
    QCOMPARE(result.reinforcementIds.size(), 8);
    QVERIFY(!result.reinforcementIds.contains(sightseeing.id));
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("杭州 景点")), &result));
    QCOMPARE(result.recallDiagnostics.mode, QStringLiteral("reuse"));
    QCOMPARE(result.recallDiagnostics.indexQueries, 0);
    QVERIFY(result.reinforcementIds.contains(sightseeing.id));
    QCOMPARE(state->calls.load(), 3);
}

void ChatPreparationExecutorTests::topicRecallAutomaticTriggerEncodesContextAndPreservesUserTopic() {
    QTemporaryDir directory;
    auto environment = environmentFor(directory);
    MemoryStore store;
    store.setDatabasePath(environment.memoryDatabasePath);
    QVERIFY(store.loadDatabaseOnly());
    const auto entry = store.addEntry(matchingMemory());
    auto state = std::make_shared<TopicProviderState>();
    TopicTestProvider provider(state);
    QVERIFY(seedTopicVector(store, entry, provider, directory.path()));
    environment.embeddingProviderFactory = [state] { return std::make_unique<TopicTestProvider>(state); };
    ChatPreparationExecutor executor;
    QVERIFY(executor.start(environment).isOk());
    ChatPreparationResult result;
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("爵士乐")), &result));
    auto automatic = requestFor(QStringLiteral("proactive_chat_tick"));
    automatic.triggerTag = QStringLiteral("proactive_chat");
    automatic.conversationMemory.append({QStringLiteral("user"), QStringLiteral("想听点即兴演奏")});
    QVERIFY(prepareOnce(executor, automatic, &result));
    QVERIFY(result.reinforcementIds.contains(entry.id));
    QCOMPARE(result.recallDiagnostics.mode, QStringLiteral("full"));
    QCOMPARE(result.recallDiagnostics.queryEmbeddingCalls, 1);
    {
        QMutexLocker locker(&state->mutex);
        QVERIFY(!state->queryTexts.last().contains(QStringLiteral("proactive_chat_tick")));
        QVERIFY(state->queryTexts.last().contains(QStringLiteral("想听点即兴演奏")));
    }
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("爵士乐")), &result));
    QCOMPARE(result.recallDiagnostics.mode, QStringLiteral("reuse"));
    automatic.conversationMemory.clear();
    QVERIFY(prepareOnce(executor, automatic, &result));
    QCOMPARE(result.recallDiagnostics.queryEmbeddingCalls, 0); // no context to encode
}

void ChatPreparationExecutorTests::topicRecallAcceptedFeedbackDrainsBeforeStop() {
    QTemporaryDir directory;
    auto environment = environmentFor(directory);
    MemoryStore store;
    store.setDatabasePath(environment.memoryDatabasePath);
    QVERIFY(store.loadDatabaseOnly());
    auto entry = store.addEntry(matchingMemory());
    {
        ChatPreparationExecutor executor;
        QVERIFY(executor.start(environment).isOk());
        QSignalSpy prepared(&executor, &ChatPreparationExecutor::prepared);
        executor.submit(requestFor());
        QVERIFY(prepared.wait(2000));
        const auto result = qvariant_cast<ChatPreparationResult>(prepared.first().first());
        executor.acceptRecallFeedback(result);
        executor.stop();
        // Destructor drains the accepted feedback ahead of the queued shutdown.
    }
    const auto snapshot = store.loadActiveMemorySnapshot();
    QCOMPARE(snapshot.items.size(), 1);
    QCOMPARE(snapshot.items.first().memoryId, entry.id);
}

void ChatPreparationExecutorTests::topicPoolRetainsMultipleTurnsAndExpiresDerivedState() {
    ActiveMemoryPool pool;
    PreparedEmbeddingQuery query;
    query.attempted = true; query.model = QStringLiteral("test"); query.vector = {1,0,0};
    const auto now = QDateTime::currentDateTimeUtc();
    ActiveMemoryItem first; first.memoryId = QStringLiteral("first"); first.activation = 0.7;
    ActiveMemoryItem second; second.memoryId = QStringLiteral("second"); second.activation = 0.6;
    pool.recordRecall(QStringLiteral("爵士乐演出"), query, false, {first}, now);
    pool.recordRecall(QStringLiteral("萨克斯音乐"), query, true, {second}, now.addSecs(1));
    QCOMPARE(pool.topicItems().size(), 2); // a memory absent last turn is still eligible
    QVERIFY(pool.assessTopic(QStringLiteral("想听点即兴演奏"), query, now.addSecs(2)).reuse);
    QCOMPARE(pool.assessTopic(QStringLiteral("想听点即兴演奏"), query, now.addSecs(1801)).reason,
             QStringLiteral("topic_idle"));
    for (int i = 0; i < ActiveMemoryPool::TOPIC_TURN_WINDOW; ++i)
        pool.recordRecall(QStringLiteral("萨克斯音乐"), query, true, {second}, now.addSecs(i+2));
    QCOMPARE(pool.topicItems().size(), 1);
    pool.clear();
    QVERIFY(pool.topicItems().isEmpty());
    QVERIFY(!pool.assessTopic(QStringLiteral("萨克斯音乐"), query, now).reuse);
    // A low-scoring candidate evicted at capacity must not reappear as a default item.
    for (int i = 0; i < ActiveMemoryPool::MAX_POOL_SIZE; ++i)
        pool.activate(QString::number(i), 1.0, QStringLiteral("session"));
    first.activation = 0.05;
    pool.recordRecall(QStringLiteral("爵士乐"), query, false, {first}, now);
    QCOMPARE(pool.activeItems().size(), ActiveMemoryPool::MAX_POOL_SIZE);
    QVERIFY(!pool.contains(first.memoryId));
}

void ChatPreparationExecutorTests::topicPoolGuardsChanges_data() {
    QTest::addColumn<QString>("previous");
    QTest::addColumn<QString>("current");
    QTest::newRow("new-facet") << QStringLiteral("杭州景点") << QStringLiteral("杭州医院");
    QTest::newRow("number") << QStringLiteral("预算2000酒店") << QStringLiteral("预算20000酒店");
    QTest::newRow("chinese-number") << QStringLiteral("预算两千酒店") << QStringLiteral("预算两万酒店");
    QTest::newRow("removed-number") << QStringLiteral("预算2000酒店") << QStringLiteral("酒店");
    QTest::newRow("negation") << QStringLiteral("喜欢咖啡") << QStringLiteral("不喜欢咖啡");
    QTest::newRow("switch") << QStringLiteral("爵士乐") << QStringLiteral("换个话题，爵士乐发展史");
    QTest::newRow("unclear-reference") << QStringLiteral("爵士乐") << QStringLiteral("它");
}
void ChatPreparationExecutorTests::topicPoolGuardsChanges() {
    QFETCH(QString, previous); QFETCH(QString, current);
    ActiveMemoryPool pool;
    PreparedEmbeddingQuery query; // no semantic provider: lexical checks still guard boundaries
    pool.recordRecall(previous, query, false, {});
    QVERIFY(!pool.assessTopic(current, query).reuse);
}

void ChatPreparationExecutorTests::topicRecallReusesCurrentCandidatesWithoutAccumulatingActivation() {
    QTemporaryDir directory;
    auto environment = environmentFor(directory);
    MemoryStore store;
    store.setDatabasePath(environment.memoryDatabasePath);
    QVERIFY(store.loadDatabaseOnly());
    auto entry = matchingMemory();
    entry.summary = entry.content = QStringLiteral("weekend jazz concerts preferred near home");
    QVERIFY(!store.addEntry(entry).id.isEmpty());
    QStringList phases;
    ChatPreparationExecutor executor;
    executor.setTestResourceLifecycleProbe([&phases](const QString& phase, quintptr) {
        // Read only after a queued prepared delivery synchronizes with the worker.
        phases.append(phase);
    });
    QVERIFY(executor.start(environment).isOk());
    ChatPreparationResult result;
    const auto request = requestFor(QStringLiteral("weekend jazz concerts preferred near home"));
    QVERIFY(prepareOnce(executor, request, &result));
    const double firstActivation = store.loadActiveMemorySnapshot().items.first().activation;
    QVERIFY(prepareOnce(executor, request, &result));
    QVERIFY(result.reinforcementIds.contains(entry.id));
    const double secondActivation = store.loadActiveMemorySnapshot().items.first().activation;
    QVERIFY2(secondActivation <= firstActivation + 1e-6, "Rescanning a candidate must not accumulate activation");
    QVERIFY2(phases.contains(QStringLiteral("recall.reuse")), "A covered same-topic request must skip complete recall");
    QCOMPARE(store.readForRecall(entry.id)->mentionCount, entry.mentionCount);
}

void ChatPreparationExecutorTests::wholeTagRecallFindsOldMemoryWithoutEmbeddings() {
    QTemporaryDir directory;
    const auto environment = environmentFor(directory); // No embedding model/provider.
    MemoryStore store;
    store.setDatabasePath(environment.memoryDatabasePath);
    QVERIFY(store.loadDatabaseOnly());
    auto old = matchingMemory();
    old.summary = QStringLiteral("那次讨论留下的独特记录");
    old.content = old.summary;
    old.tags = {QStringLiteral("人工智能研究")};
    old.createdAt = QDateTime::currentDateTimeUtc().addDays(-100);
    old.updatedAt = old.createdAt;
    old = store.addEntry(old);
    QVERIFY(fillRecentWindow(store));
    ChatPreparationExecutor executor;
    QVERIFY(executor.start(environment).isOk());
    ChatPreparationResult result;
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("人工智能研究")), &result));
    QVERIFY(result.messages.last().content.contains(old.summary));
    QVERIFY(result.reinforcementIds.contains(old.id));
    old.privacyLevel = PrivacyLevel::Sensitive;
    QVERIFY(store.updateEntryById(old));
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("人工智能研究")), &result));
    QVERIFY(!result.messages.last().content.contains(old.summary));
    old.privacyLevel = PrivacyLevel::Personal;
    old.tags = {QStringLiteral("分布式数据库系统")};
    QVERIFY(store.updateEntryById(old));
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("人工智能研究")), &result));
    QVERIFY(!result.messages.last().content.contains(old.summary));
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("分布式数据库系统")), &result));
    QVERIFY(result.messages.last().content.contains(old.summary));
    QVERIFY(store.updateStatusById(old.id, MemoryStatus::Consolidated));
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("分布式数据库系统")), &result));
    QVERIFY(!result.messages.last().content.contains(old.summary));
    store.clear();
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("分布式数据库系统")), &result));
    QVERIFY(result.reinforcementIds.isEmpty());
}

void ChatPreparationExecutorTests::semanticRecallFindsOldMemoryAndRejectsStalePrivateHits() {
    QTemporaryDir directory;
    const auto environment = environmentFor(directory);
    MemoryStore store;
    store.setDatabasePath(environment.memoryDatabasePath);
    QVERIFY(store.loadDatabaseOnly());
    auto old = matchingMemory();
    old.createdAt = QDateTime::currentDateTimeUtc().addDays(-100);
    old.updatedAt = old.createdAt;
    old = store.addEntry(old);
    QVERIFY(!old.id.isEmpty());
    QVERIFY(fillRecentWindow(store));
    // Persist a long-term vector before starting the background owner.
    WorkerTestProvider seedProvider;
    {
        HnswEmbeddingIndex index(store.databaseConnectionName(), &seedProvider, directory.path());
        const auto vector = seedProvider.embed(old.summary);
        QSqlQuery persisted(QSqlDatabase::database(store.databaseConnectionName(), false));
        persisted.prepare(QStringLiteral("INSERT OR REPLACE INTO memory_embeddings(memory_id,model,dimension,vector_blob,content_hash) VALUES(?,?,?,?,?)"));
        persisted.addBindValue(old.id);
        persisted.addBindValue(seedProvider.modelName());
        persisted.addBindValue(vector.size());
        persisted.addBindValue(QByteArray(reinterpret_cast<const char*>(vector.constData()), vector.size() * sizeof(float)));
        const auto hash = HnswEmbeddingIndex::contentHash(old.summary + QLatin1Char('\n') + old.content);
        persisted.addBindValue(hash);
        QVERIFY(persisted.exec());
        QVERIFY(index.upsertVector(old.id, vector, hash));
        QVERIFY(index.saveToDisk());
    }
    ChatPreparationEnvironment configured = environment;
    configured.embeddingProviderFactory = [] { return std::make_unique<WorkerTestProvider>(); };
    ChatPreparationExecutor executor;
    QVERIFY(executor.start(configured).isOk());
    ChatPreparationResult result;
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("different wording")), &result));
    QVERIFY(result.reinforcementIds.contains(old.id));
    QVERIFY(result.messages.last().content.contains(old.summary));

    // A live stale HNSW hit and cached activation must not expose changed data.
    old.privacyLevel = PrivacyLevel::Sensitive;
    QVERIFY(store.updateEntryById(old));
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("different wording")), &result));
    QVERIFY(!result.reinforcementIds.contains(old.id));
    QVERIFY(!result.messages.last().content.contains(old.summary));
    QVERIFY(store.removeEntryById(old.id));
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("different wording")), &result));
    QVERIFY(!result.reinforcementIds.contains(old.id));
}

void ChatPreparationExecutorTests::emotionBonusIsNotPersistedIntoActivationPool() {
    QList<double> activations;
    for (const auto emotion : {EmotionType::Neutral, EmotionType::Joy}) {
        QTemporaryDir directory;
        const auto environment = environmentFor(directory);
        MemoryStore store;
        store.setDatabasePath(environment.memoryDatabasePath);
        QVERIFY(store.loadDatabaseOnly());
        auto memory = matchingMemory();
        memory.emotion = EmotionType::Joy;
        memory.emotionIntensity = memory.emotionConfidence = 1.0;
        QVERIFY(!store.addEntry(memory).id.isEmpty());
        {
            ChatPreparationExecutor executor;
            QVERIFY(executor.start(environment).isOk());
            auto request = requestFor();
            EmotionSnapshot mood;
            mood.active = emotion;
            mood.intensity = 1.0;
            mood.updatedAt = QDateTime::currentDateTimeUtc();
            request.emotion = mood;
            ChatPreparationResult result;
            QVERIFY(prepareOnce(executor, request, &result));
            QVERIFY(result.reinforcementIds.contains(memory.id));
        }
        const auto snapshot = store.loadActiveMemorySnapshot();
        QCOMPARE(snapshot.items.size(), 1);
        activations.append(snapshot.items.first().activation);
    }
    QVERIFY(qAbs(activations[0] - activations[1]) < 1e-12);
}

void ChatPreparationExecutorTests::activePoolSurvivesWorkerRestartAndClear() {
    QTemporaryDir directory;
    const auto environment = environmentFor(directory);
    MemoryStore store;
    store.setDatabasePath(environment.memoryDatabasePath);
    QVERIFY(store.loadDatabaseOnly());
    const auto old = store.addEntry(matchingMemory());
    {
        ChatPreparationExecutor executor;
        QVERIFY(executor.start(environment).isOk());
        ChatPreparationResult result;
        QVERIFY(prepareOnce(executor, requestFor(), &result));
        QVERIFY(result.reinforcementIds.contains(old.id));
    }
    const auto saved = store.loadActiveMemorySnapshot();
    QCOMPARE(saved.items.size(), 1);
    QCOMPARE(saved.items.first().memoryId, old.id);
    QVERIFY(fillRecentWindow(store));
    ChatPreparationExecutor restarted;
    QVERIFY(restarted.start(environment).isOk());
    ChatPreparationResult result;
    QVERIFY(prepareOnce(restarted, requestFor(QStringLiteral("爵士乐")), &result));
    QVERIFY(result.reinforcementIds.contains(old.id));
    store.clear();
    QVERIFY(prepareOnce(restarted, requestFor(QStringLiteral("爵士乐")), &result));
    QVERIFY(result.reinforcementIds.isEmpty());
    QVERIFY(store.loadActiveMemorySnapshot().isEmpty());
    // Importing the same ID after clear must not resurrect its old activation.
    QVERIFY(!store.addEntry(old).id.isEmpty());
    QVERIFY(fillRecentWindow(store));
    QVERIFY(prepareOnce(restarted, requestFor(QStringLiteral("爵士乐")), &result));
    QVERIFY(!result.reinforcementIds.contains(old.id));
    QVERIFY(!result.messages.last().content.contains(old.summary));
}

void ChatPreparationExecutorTests::slowProviderInitializationDoesNotBlockGui() {
    QTemporaryDir directory;
    auto environment = environmentFor(directory);
    MemoryStore store;
    store.setDatabasePath(environment.memoryDatabasePath);
    QVERIFY(store.loadDatabaseOnly());
    QVERIFY(!store.addEntry(matchingMemory()).id.isEmpty());
    auto released = std::make_shared<std::atomic_bool>(false);
    auto wrongThread = std::make_shared<std::atomic_bool>(false);
    const auto gui = reinterpret_cast<quintptr>(QThread::currentThreadId());
    environment.embeddingProviderFactory = [released, wrongThread, gui] {
        if (reinterpret_cast<quintptr>(QThread::currentThreadId()) == gui) wrongThread->store(true);
        QElapsedTimer timeout;
        timeout.start();
        while (!released->load() && timeout.elapsed() < 2000) QThread::msleep(1);
        return std::make_unique<WorkerTestProvider>(wrongThread, gui);
    };
    {
        ChatPreparationExecutor executor;
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(executor.start(environment).isOk());
        QVERIFY2(elapsed.elapsed() < 500, "start blocked on provider initialization");
        QTimer::singleShot(0, [released] { released->store(true); });
        ChatPreparationResult result;
        QVERIFY(prepareOnce(executor, requestFor(), &result));
        QVERIFY(result.reinforcementIds.contains(QStringLiteral("memory-jazz")));
    }
    QVERIFY(released->load());
    QVERIFY(!wrongThread->load());
}

#ifdef DESKTOP_PET_HAS_ORT
void ChatPreparationExecutorTests::nativeOnnxRecallRunsThroughChatWorker() {
    QTemporaryDir directory;
    auto environment = environmentFor(directory);
    environment.embeddingAssetsDirectory = QDir(QStringLiteral(DESKTOP_PET_EMBEDDING_ASSETS)).absoluteFilePath(QStringLiteral(".."));
    QVERIFY(QFileInfo::exists(QDir(environment.embeddingAssetsDirectory).filePath(QStringLiteral("embeddings/model_quantized.onnx"))));
    MemoryStore store;
    store.setDatabasePath(environment.memoryDatabasePath);
    QVERIFY(store.loadDatabaseOnly());
    auto old = matchingMemory();
    old.createdAt = QDateTime::currentDateTimeUtc().addDays(-100);
    old.updatedAt = old.createdAt;
    old = store.addEntry(old);
    QVERIFY(!old.id.isEmpty());
    QVERIFY(fillRecentWindow(store));
    QSqlQuery prioritize(QSqlDatabase::database(store.databaseConnectionName(), false));
    prioritize.prepare(QStringLiteral("UPDATE memory_index_jobs SET created_at='2000-01-01T00:00:00Z' WHERE memory_id=:id"));
    prioritize.bindValue(QStringLiteral(":id"), old.id);
    QVERIFY(prioritize.exec());
    ChatPreparationExecutor executor;
    QVERIFY(executor.start(environment).isOk());
    ChatPreparationResult result;
    // The old preference is absent from both recency windows and the fresh pool.
    // Maintenance and query embedding must both run in the worker for this to hit.
    QVERIFY(prepareOnce(executor, requestFor(QStringLiteral("我爱听即兴演奏的音乐")), &result));
    QVERIFY(result.reinforcementIds.contains(old.id));
    QVERIFY(result.messages.last().content.contains(old.summary));
}
#endif


void ChatPreparationExecutorTests::avatarDescriptionAndStateRemainSeparateFromIdentity() {
    QTemporaryDir directory;ChatPreparationExecutor executor;
    auto environment=environmentFor(directory);QVERIFY(executor.start(environment).isOk());
    auto request=requestFor();
    request.avatarContext={{"state",QJsonObject{{"current_state","cloud_perch"},{"visible",false}}},{"avatar",QJsonObject{{"description","a tiny blue bird with a yellow scarf"}}}};
    const auto originalProfile=request.runtimeMetadata.profileId;
    ChatPreparationResult result;QVERIFY(prepareOnce(executor,request,&result));
    QString text;for(const auto& m:result.messages)text+=m.content;
    QVERIFY(text.contains("cloud_perch"));QVERIFY(text.contains("a tiny blue bird with a yellow scarf"));
    QVERIFY(!text.contains("pet_state=Idle"));QCOMPARE(request.runtimeMetadata.profileId,originalProfile);
    executor.stop();
}

void ChatPreparationExecutorTests::prepare_externalContextFiltersWorkingMemoryBeforeQuery() {
    QTemporaryDir directory;
    auto environment = environmentFor(directory);
    MemoryStore store;
    store.setProfileId(environment.profileId);
    store.setDatabasePath(environment.memoryDatabasePath);
    QVERIFY(store.loadDatabaseOnly());
    MemoryEntry publicMemory;
    publicMemory.id = "public-cue";
    publicMemory.type = MemoryType::Semantic;
    publicMemory.summary = publicMemory.content = "金丝雀线索只应在明确询问时召回";
    publicMemory.attribution.profileId = environment.profileId;
    publicMemory.attribution.audience.kind = MemoryAudienceKind::Shareable;
    QVERIFY(!store.addEntry(publicMemory).id.isEmpty());
    ChatPreparationExecutor executor;
    QVERIFY(executor.start(environment));
    auto request = requestFor("继续");
    ContextSpec context;
    context.profileId = environment.profileId;
    context.originKind = OriginKind::External;
    context.conversationId = "B";
    request.context = context;
    request.runtimeMetadata.originKind = OriginKind::External;
    WorkingMemoryItem hidden;
    hidden.id = "private-tool";
    hidden.source = "user_task";
    hidden.summary = hidden.content = "金丝雀线索";
    hidden.attribution.profileId = environment.profileId;
    hidden.attribution.audience = {MemoryAudienceKind::Conversation, {"A"}};
    request.workingMemory = {hidden};
    ChatPreparationResult result;
    QVERIFY(prepareOnce(executor, request, &result));
    for (const auto& message : result.messages)
        QVERIFY(!message.content.contains("金丝雀线索"));
    request.requestId = "explicit-public";
    request.reason = "金丝雀线索";
    QVERIFY(prepareOnce(executor, request, &result));
    bool found = false;
    for (const auto& message : result.messages)
        found = found || message.content.contains(publicMemory.content);
    QVERIFY(found);
    executor.stop();
}

QTEST_MAIN(ChatPreparationExecutorTests)
#include "test_chat_preparation_executor.moc"
