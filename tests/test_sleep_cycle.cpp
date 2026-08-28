#include <QtTest>

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <utility>

#include "ai/ai_brain.h"
#include "ai/context/context_assembler.h"
#include "ai/memory/daydream_consolidator.h"
#include "ai/memory/memory_store.h"
#include "ai/model/model_role_registry.h"
#include "ai/model/model_router.h"
#include "ai/reflection/daydream_sleep_adapter.h"
#include "ai/reflection/diary_service.h"
#include "ai/reflection/inner_thought_service.h"
#include "ai/reflection/private_key_provider.h"
#include "ai/reflection/private_psyche_crypto.h"
#include "ai/reflection/sleep_cycle_coordinator.h"
#include "ai/reflection/sleep_session_repository.h"
#include "ai/reflection/sqlite_private_psyche_repository.h"
#include "ai/scheduler/agent_scheduler.h"
#include "configLoader/config_manager.h"

namespace {

const QString kProfileId = QStringLiteral("11111111-1111-4111-8111-111111111111");
const QDate kLocalDate(2026, 8, 25);

ModelRouteConfig route(const QString& id) {
    ModelRouteConfig config;
    config.routeId = id;
    config.enabled = true;
    config.llm.enabled = true;
    config.llm.provider = QStringLiteral("test");
    config.llm.baseUrl = QStringLiteral("https://models.example/v1");
    config.llm.apiKey = QStringLiteral("test-key");
    config.llm.model = QStringLiteral("test-model");
    config.llm.timeoutMs = 1000;
    return config;
}

ModelRoleConfig roleConfig(ModelRole role) {
    ModelRoleConfig config;
    config.role = role;
    config.routes = {route(QString::number(static_cast<int>(role)))};
    return config;
}

class FakeModelClient final : public ModelCompletionClient {
public:
    struct Reply {
        bool success = true;
        QString content;
        QString error;
        QString reasoning;
    };

    QList<Reply> replies;
    QList<QString> selectedRouteIds;
    QList<QList<ChatMessage>> requests;
    bool deferCallbacks = false;

    struct PendingCompletion {
        Reply reply;
        LlmCompletionHandler callback;
    };
    QList<PendingCompletion> pending;

    void completeOnce(const ModelRouteConfig& selectedRoute,
                      const QList<ChatMessage>& messages,
                      const QJsonArray& tools,
                      LlmCompletionHandler callback,
                      const QString& petName) override {
        Q_UNUSED(tools)
        Q_UNUSED(petName)
        selectedRouteIds.append(selectedRoute.routeId);
        requests.append(messages);
        if (replies.isEmpty()) {
            callback(false, {}, QStringLiteral("missing fake reply"));
            return;
        }
        const Reply reply = replies.takeFirst();
        if (deferCallbacks) {
            pending.append(PendingCompletion{reply, std::move(callback)});
            return;
        }
        deliver(reply, std::move(callback));
    }

    void finishNext() {
        if (pending.isEmpty()) return;
        PendingCompletion completion = pending.takeFirst();
        deliver(completion.reply, std::move(completion.callback));
    }

private:
    static void deliver(const Reply& reply, LlmCompletionHandler callback) {
        LlmResponse response;
        response.content = reply.content;
        response.reasoningContent = reply.reasoning;
        callback(reply.success, response, reply.error);
    }
};

class TestKeyProvider final : public PrivateKeyProvider {
public:
    bool available = true;
    QByteArray key = QByteArray(32, 'k');

    Result<PrivateKeyMaterial, DomainError> loadOrCreate(
        const QString& profileId) override {
        if (!available) {
            return Result<PrivateKeyMaterial, DomainError>::failure(
                domainError(QStringLiteral("PRIVATE_STORE_UNAVAILABLE"),
                            QStringLiteral("test key unavailable")));
        }
        return Result<PrivateKeyMaterial, DomainError>::success(
            PrivateKeyMaterial{profileId, 1, key});
    }
};

class TestCrypto final : public PrivatePsycheCrypto {
public:
    Result<EncryptedPrivatePayload, DomainError> encrypt(
        const QByteArray& plaintext,
        const PrivateRecordAad& aad,
        const PrivateKeyMaterial& key) const override {
        EncryptedPrivatePayload encrypted;
        encrypted.schemaVersion = aad.schemaVersion;
        encrypted.keyVersion = key.keyVersion;
        encrypted.nonce = QByteArrayLiteral("test-nonce");
        const QByteArray stream = QCryptographicHash::hash(
            key.key + aad.toBytes() + encrypted.nonce,
            QCryptographicHash::Sha256);
        QByteArray ciphertext = plaintext;
        for (qsizetype i = 0; i < ciphertext.size(); ++i) {
            ciphertext[i] = static_cast<char>(
                static_cast<unsigned char>(ciphertext.at(i))
                ^ static_cast<unsigned char>(stream.at(i % stream.size())));
        }
        const QByteArray tag = QCryptographicHash::hash(
            key.key + aad.toBytes() + encrypted.nonce + plaintext,
            QCryptographicHash::Sha256);
        encrypted.ciphertext = ciphertext + tag;
        return Result<EncryptedPrivatePayload, DomainError>::success(encrypted);
    }

    Result<QByteArray, DomainError> decrypt(
        const EncryptedPrivatePayload& encrypted,
        const PrivateRecordAad& aad,
        const PrivateKeyMaterial& key) const override {
        if (encrypted.ciphertext.size() < 32) {
            return Result<QByteArray, DomainError>::failure(
                domainError(QStringLiteral("PRIVATE_AUTH_FAILED"),
                            QStringLiteral("ciphertext is truncated")));
        }
        QByteArray plaintext = encrypted.ciphertext.left(
            encrypted.ciphertext.size() - 32);
        const QByteArray actualTag = encrypted.ciphertext.right(32);
        const QByteArray stream = QCryptographicHash::hash(
            key.key + aad.toBytes() + encrypted.nonce,
            QCryptographicHash::Sha256);
        for (qsizetype i = 0; i < plaintext.size(); ++i) {
            plaintext[i] = static_cast<char>(
                static_cast<unsigned char>(plaintext.at(i))
                ^ static_cast<unsigned char>(stream.at(i % stream.size())));
        }
        const QByteArray expectedTag = QCryptographicHash::hash(
            key.key + aad.toBytes() + encrypted.nonce + plaintext,
            QCryptographicHash::Sha256);
        if (actualTag != expectedTag) {
            return Result<QByteArray, DomainError>::failure(
                domainError(QStringLiteral("PRIVATE_AUTH_FAILED"),
                            QStringLiteral("private record authentication failed")));
        }
        return Result<QByteArray, DomainError>::success(plaintext);
    }
};

MemoryEntry addInboxAt(MemoryStore& store,
                       const QDateTime& timestamp,
                       const QString& content) {
    MemoryEntry entry;
    entry.type = MemoryType::Episodic;
    entry.partition = QStringLiteral("hippocampus");
    entry.status = MemoryStatus::Active;
    entry.privacyLevel = PrivacyLevel::Personal;
    entry.key = QStringLiteral("inbox:test");
    entry.summary = content;
    entry.content = content;
    entry.source = QStringLiteral("user");
    entry.importance = 0.9;
    entry.mentionCount = 2;
    entry.createdAt = timestamp;
    entry.updatedAt = entry.createdAt;
    return store.addEntry(entry);
}

MemoryEntry addInbox(MemoryStore& store,
                     const QString& content = QStringLiteral("owner likes tea")) {
    return addInboxAt(
        store, QDateTime::currentDateTimeUtc().addSecs(-60), content);
}

QString innerThoughtJson(bool includeReasoning = false) {
    QJsonObject object{
        {QStringLiteral("appraisal"), QStringLiteral("这次交流很重要")},
        {QStringLiteral("desire"), QStringLiteral("下次继续倾听")},
        {QStringLiteral("uncertainty"), QStringLiteral("不确定对方是否疲惫")},
        {QStringLiteral("unresolved"), true}
    };
    if (includeReasoning) {
        object.insert(QStringLiteral("reasoning_trace"),
                      QStringLiteral("raw hidden reasoning must not persist"));
    }
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString diaryJson(const QString& body = QStringLiteral("今天和主人聊了喝茶的事。")) {
    const QJsonObject object{
        {QStringLiteral("body"), body},
        {QStringLiteral("index"), QJsonObject{
             {QStringLiteral("themes"), QJsonArray{QStringLiteral("陪伴")}},
             {QStringLiteral("unresolved"), QJsonArray{}}}}
    };
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString discardDecisionJson(const QString& sourceId) {
    const QJsonArray decisions{QJsonObject{
        {QStringLiteral("source_id"), sourceId},
        {QStringLiteral("action"), QStringLiteral("discard")},
        {QStringLiteral("quality_score"), 1},
        {QStringLiteral("new_tags"), QJsonArray{}}
    }};
    return QString::fromUtf8(
        QJsonDocument(decisions).toJson(QJsonDocument::Compact));
}

void useLegacySecondPrecision(QJsonObject* entry) {
    const QStringList timestampKeys{
        QStringLiteral("created_at"), QStringLiteral("updated_at"),
        QStringLiteral("last_accessed_at"), QStringLiteral("expires_at")};
    for (const QString& key : timestampKeys) {
        const QDateTime timestamp = QDateTime::fromString(
            entry->value(key).toString(), Qt::ISODateWithMs);
        if (timestamp.isValid()) {
            entry->insert(key, timestamp.toString(Qt::ISODate));
        }
    }
}

QJsonObject legacySecondPrecisionChangeSet(const DaydreamChangeSet& changeSet) {
    QJsonObject legacy = changeSet.toJson();
    QJsonArray snapshot = legacy.value(QStringLiteral("snapshot")).toArray();
    for (qsizetype i = 0; i < snapshot.size(); ++i) {
        QJsonObject entry = snapshot.at(i).toObject();
        useLegacySecondPrecision(&entry);
        snapshot.replace(i, entry);
    }
    QJsonArray decisions = legacy.value(QStringLiteral("decisions")).toArray();
    for (qsizetype i = 0; i < decisions.size(); ++i) {
        QJsonObject decision = decisions.at(i).toObject();
        if (decision.value(QStringLiteral("expectedTarget")).isObject()) {
            QJsonObject target = decision.value(
                QStringLiteral("expectedTarget")).toObject();
            useLegacySecondPrecision(&target);
            decision.insert(QStringLiteral("expectedTarget"), target);
            decisions.replace(i, decision);
        }
    }
    legacy.insert(QStringLiteral("snapshot"), snapshot);
    legacy.insert(QStringLiteral("decisions"), decisions);
    const QJsonObject content{
        {QStringLiteral("snapshot"), snapshot},
        {QStringLiteral("decisions"), decisions}};
    legacy.insert(QStringLiteral("changeSetId"), QString::fromLatin1(
        QCryptographicHash::hash(
            QJsonDocument(content).toJson(QJsonDocument::Compact),
            QCryptographicHash::Sha256).toHex()));
    return legacy;
}

struct ReflectionFixture {
    QTemporaryDir directory;
    MemoryStore memory;
    SqlitePrivatePsycheRepository privateRepository;
    SleepSessionRepository sleepSessions;
    TestKeyProvider keys;
    TestCrypto crypto;
    FakeModelClient modelClient;
    ModelRoleRegistry registry{
        {roleConfig(ModelRole::FastExtract),
         roleConfig(ModelRole::Consolidation),
         roleConfig(ModelRole::Daydream),
         roleConfig(ModelRole::Diary)}};
    ModelRouter router{&registry, &modelClient};
    ContextAssembler assembler;

    bool open() {
        if (!directory.isValid()) return false;
        memory.setDatabasePath(directory.filePath(QStringLiteral("memory.db")));
        memory.setStoragePath(directory.filePath(QStringLiteral("memory.json")));
        QString error;
        return memory.load(&error)
            && privateRepository.open(
                   directory.filePath(QStringLiteral("private_psyche.sqlite"))).isOk()
            && sleepSessions.open(
                   directory.filePath(QStringLiteral("agent_runtime.sqlite"))).isOk();
    }

    InnerThoughtService innerThoughtService() {
        return InnerThoughtService(
            kProfileId, &router, &keys, &crypto, &privateRepository);
    }

    DiaryService diaryService(ModelRole readRole = ModelRole::Diary) {
        return DiaryService(
            kProfileId, &router, &assembler, &keys, &crypto,
            &privateRepository, readRole);
    }

    DaydreamSleepAdapter daydreamAdapter() {
        return DaydreamSleepAdapter(kProfileId, QStringLiteral("Milltina"),
                                    &memory, &router);
    }
};

InnerThoughtRequest innerRequest() {
    InnerThoughtRequest request;
    request.profileId = kProfileId;
    request.sourceEventId = QStringLiteral("22222222-2222-4222-8222-222222222222");
    request.contextSnapshot = {
        {QStringLiteral("event"), QStringLiteral("owner shared a concern")}};
    request.privacy = EventPrivacy::Private;
    return request;
}

DiaryRequest diaryRequest(const QString& sessionId) {
    DiaryRequest request;
    request.profileId = kProfileId;
    request.sessionId = sessionId;
    request.localDate = kLocalDate;
    request.sourceCutoffSequence = 12;
    request.petName = QStringLiteral("Milltina");
    request.eventSummaries = {QStringLiteral("owner likes tea")};
    return request;
}

bool replaceDiaryCiphertext(SqlitePrivatePsycheRepository& repository,
                            const QString& entryId,
                            const QByteArray& ciphertext) {
    QSqlDatabase database = QSqlDatabase::database(repository.connectionName());
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "UPDATE diary_entry SET ciphertext=? WHERE entry_id=?"));
    query.addBindValue(ciphertext);
    query.addBindValue(entryId);
    return query.exec() && query.numRowsAffected() == 1;
}

bool replaceDiaryProfile(SqlitePrivatePsycheRepository& repository,
                         const QString& entryId,
                         const QString& profileId) {
    QSqlDatabase database = QSqlDatabase::database(repository.connectionName());
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "UPDATE diary_entry SET profile_id=? WHERE entry_id=?"));
    query.addBindValue(profileId);
    query.addBindValue(entryId);
    return query.exec() && query.numRowsAffected() == 1;
}

void queueSleepReplies(ReflectionFixture& fixture) {
    fixture.modelClient.replies.append(
        {false, {}, QStringLiteral("use deterministic consolidation fallback"), {}});
    fixture.modelClient.replies.append({true, diaryJson(), {}, {}});
}

bool prepareSleepParticipants(ReflectionFixture& fixture,
                              const QString& sessionId,
                              DaydreamSleepAdapter& adapter,
                              DiaryService& diary) {
    queueSleepReplies(fixture);
    CancellationSource cancellation;
    const CancellationToken token = cancellation.token();
    StagingSession staging{sessionId, token.generation()};
    bool memoryPrepared = false;
    adapter.consolidateAsync(
        {kProfileId, sessionId, 12, 32}, staging, token,
        [&](Result<DaydreamChangeSet, DomainError> result) {
            memoryPrepared = result.isOk();
        });
    bool diaryPrepared = false;
    diary.composeAsync(
        diaryRequest(sessionId), staging, token,
        [&](Result<QString, DomainError> result) {
            diaryPrepared = result.isOk();
        });
    return memoryPrepared && diaryPrepared
        && adapter.preparedChangeCount(sessionId) == 1
        && fixture.privateRepository.preparedDiaryCount(sessionId) == 1;
}

QString writeConfig(QTemporaryDir& directory, const QJsonObject& root) {
    const QString path = directory.filePath(QStringLiteral("sleep-config.json"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.close();
    return path;
}

} // namespace

class TestIdentityState {
public:
    static void runPreparedDaydreamBatch(AIBrain& brain) {
        brain.m_daydreamConfig.idleThresholdSec = -1;
        brain.m_daydreamConfig.dueSoonThresholdMs = 0;
        brain.m_daydreamConfig.batchLimit = DaydreamConsolidator::BATCH_LIMIT;
        brain.m_daydreamConfig.relatedMemoryLimit = 8;
        brain.m_daydreamPolicy.configure(brain.m_daydreamConfig);
        brain.m_running = true;
        brain.m_busy = false;
        brain.m_externalSleepCoordinatorEnabled = false;
        brain.m_daydreamRunning = true;
        ++brain.m_daydreamGeneration;
        DaydreamConsolidator consolidator(brain.m_memoryStore);
        brain.m_daydreamSnapshot = consolidator.createSnapshot();
        brain.m_daydreamDecisions.clear();
        brain.m_daydreamBatchOffset = 0;
        brain.m_daydreamFallbackBatches = 0;
        brain.m_daydreamInvalidBatches = 0;
        brain.runNextDaydreamBatch(brain.m_daydreamGeneration);
    }
};

class SleepCycleTests : public QObject {
    Q_OBJECT

private slots:
    void createAsync_whenHighValueEventCompletes_shouldStageShortPrivateSummaryWithoutBlockingReply();
    void createAsync_whenCallbackArrivesAfterCancellation_shouldDiscardResult();
    void createAsync_whenModelReturnsReasoningTrace_shouldPersistOnlyRequestedSummaryFields();

    void consolidateAsync_whenPendingItemsExist_shouldReuseExistingConsolidatorAndStageBoundedChanges();
    void consolidateAsync_beforeCommit_shouldLeaveFormalMemoryUnchanged();
    void processNextBatch_whenModelDecisionIsRequired_shouldRequestDaydreamRoleAndStageChangeSet();
    void processNextBatch_whenCallbackArrivesAfterCancellation_shouldDiscardLateResult();

    void runNextDaydreamBatch_whenModelDecisionIsRequired_shouldRequestDaydreamRoleAndApplyDecision();
    void runNextDaydreamBatch_whenDaydreamRoutesFail_shouldUseBoundedHardcodedFallback();

    void buildChangeSet_whenDecisionsAreValid_shouldReturnDeterministicChangesWithoutMutatingStore();
    void applyChangeSet_whenCommitIsDurable_shouldMaterializeChangesOnce();
    void finalizeSession_whenLegacySecondPrecisionChangeSetIsLoaded_shouldRecover();
    void applyDecisions_whenCalledByLegacyDaydream_shouldDelegateAndPreserveExistingBehavior();

    void composeAsync_whenBedtimeContextIsValid_shouldStageOneEncryptedDiaryForLocalDate();
    void composeAsync_whenDateAlreadyCommitted_shouldReturnExistingEntryId();
    void composeAsync_whenModelOutputIsEmptyAfterRepair_shouldKeepRetryablePendingState();
    void composeAsync_whenKeychainUnavailable_shouldReturnPrivateStoreUnavailableWithoutPlaintextFallback();
    void privateServices_whenBuildCapabilityIsUnavailable_shouldReturnPrivateStoreUnavailableWithoutPlaintextFallback();

    void readForSelf_whenDiaryRoleHasEntryScope_shouldDecryptSelectedEntry();
    void readForSelf_whenDialogueScopeRequestsEntry_shouldReturnScopeDenied();
    void readForOwner_whenAuthAndProfileMatch_shouldDecryptSingleEntry();
    void readForOwner_whenAadOrCiphertextIsModified_shouldRejectAuthentication();

    void tryStart_whenBedtimeIdleAndNoDueTask_shouldCreateDurablePendingSession();
    void tryStart_whenBrainBusyOrTaskDueSoon_shouldNotStart();
    void tryStart_whenDiaryAlreadyCommitted_shouldSkipBedtimeButAllowManual();
    void tryStart_whenAllParticipantsPrepared_shouldPersistCommitThenFinalizeAllStores();
    void tryStart_whenRestartFindsCommittedSession_shouldIdempotentlyFinishFinalize();
    void cancel_whenDecisionPending_shouldAbortAllStagingAndPreserveFormalState();
    void cancel_whenDecisionCommitted_shouldKeepCommitAndFinishFinalize();
    void recoverIncomplete_whenCommittedSessionExists_shouldFinalizeBeforePublishingCapability();
    void start_whenCoordinatorTakesOwnership_shouldDisableLegacyDaydreamTimer();
    void stop_whenCallbacksAreLate_shouldInvalidateGenerationAndPerformNoWrites();

    void getSleepPolicy_whenConfigured_shouldReturnSanitizedPolicy();
    void getSleepPolicy_whenMissingOrInvalid_shouldUseSafeDefaults();
};

void SleepCycleTests::createAsync_whenHighValueEventCompletes_shouldStageShortPrivateSummaryWithoutBlockingReply() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    fixture.modelClient.replies.append({true, innerThoughtJson(), {}, {}});
    InnerThoughtService service = fixture.innerThoughtService();
    CancellationSource cancellation;
    bool completed = false;
    service.createAsync(innerRequest(), cancellation.token(),
                        [&](Result<QString, DomainError> result) {
        QVERIFY(result.isOk());
        completed = true;
    });
    QVERIFY(completed);
    QCOMPARE(fixture.privateRepository.innerThoughtCount(kProfileId), 1);
}

void SleepCycleTests::createAsync_whenCallbackArrivesAfterCancellation_shouldDiscardResult() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    fixture.modelClient.replies.append({true, innerThoughtJson(), {}, {}});
    fixture.modelClient.deferCallbacks = true;
    InnerThoughtService service = fixture.innerThoughtService();
    CancellationSource cancellation;
    const CancellationToken token = cancellation.token();
    bool completed = false;
    service.createAsync(innerRequest(), token,
                        [&](Result<QString, DomainError> result) {
        QVERIFY(!result.isOk());
        QCOMPARE(result.error().code, QStringLiteral("SLEEP_CANCELLED"));
        completed = true;
    });
    QVERIFY(!completed);
    QCOMPARE(fixture.modelClient.pending.size(), 1);
    cancellation.cancel();
    fixture.modelClient.finishNext();
    QVERIFY(completed);
    QCOMPARE(fixture.privateRepository.innerThoughtCount(kProfileId), 0);
}

void SleepCycleTests::createAsync_whenModelReturnsReasoningTrace_shouldPersistOnlyRequestedSummaryFields() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    fixture.modelClient.replies.append(
        {true, innerThoughtJson(true), {}, QStringLiteral("raw reasoning")});
    InnerThoughtService service = fixture.innerThoughtService();
    CancellationSource cancellation;
    QString entryId;
    service.createAsync(innerRequest(), cancellation.token(),
                        [&](Result<QString, DomainError> result) {
        QVERIFY(result.isOk());
        entryId = result.value();
    });
    const auto summary = service.readSummary(entryId);
    QVERIFY(summary.isOk());
    QVERIFY(!summary.value().appraisal.contains(QStringLiteral("reasoning")));
    QVERIFY(!summary.value().uncertainty.contains(QStringLiteral("raw")));
}

void SleepCycleTests::consolidateAsync_whenPendingItemsExist_shouldReuseExistingConsolidatorAndStageBoundedChanges() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    addInbox(fixture.memory);
    DaydreamSleepAdapter adapter = fixture.daydreamAdapter();
    StagingSession staging{QStringLiteral("sleep-1"), 1};
    DaydreamRequest request{kProfileId, staging.sessionId, 0, 1};
    CancellationSource cancellation;
    bool completed = false;
    adapter.consolidateAsync(request, staging, cancellation.token(),
                             [&](Result<DaydreamChangeSet, DomainError> result) {
        QVERIFY(result.isOk());
        QCOMPARE(result.value().snapshot.size(), 1);
        completed = true;
    });
    QVERIFY(completed);
    QCOMPARE(fixture.memory.preparedSleepChangeCount(staging.sessionId), 1);
}

void SleepCycleTests::consolidateAsync_beforeCommit_shouldLeaveFormalMemoryUnchanged() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    const MemoryEntry source = addInbox(fixture.memory);
    DaydreamSleepAdapter adapter = fixture.daydreamAdapter();
    StagingSession staging{QStringLiteral("sleep-2"), 1};
    CancellationSource cancellation;
    adapter.consolidateAsync({kProfileId, staging.sessionId, 0, 1}, staging,
                             cancellation.token(), [](auto) {});
    QVERIFY(fixture.memory.findById(source.id));
    QCOMPARE(fixture.memory.preparedSleepChangeCount(staging.sessionId), 1);
}

void SleepCycleTests::processNextBatch_whenModelDecisionIsRequired_shouldRequestDaydreamRoleAndStageChangeSet() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    const MemoryEntry source = addInbox(fixture.memory);
    fixture.modelClient.replies.append(
        {true, discardDecisionJson(source.id), {}, {}});
    DaydreamSleepAdapter adapter = fixture.daydreamAdapter();
    CancellationSource cancellation;
    const CancellationToken token = cancellation.token();
    StagingSession staging{QStringLiteral("sleep-daydream-route"),
                           token.generation()};
    bool completed = false;

    adapter.consolidateAsync(
        {kProfileId, staging.sessionId, 0, 1}, staging, token,
        [&](Result<DaydreamChangeSet, DomainError> result) {
            QVERIFY(result.isOk());
            completed = true;
        });

    QVERIFY(completed);
    QCOMPARE(fixture.modelClient.selectedRouteIds,
             QList<QString>{QString::number(
                 static_cast<int>(ModelRole::Daydream))});
    QCOMPARE(adapter.preparedChangeCount(staging.sessionId), 1);
}

void SleepCycleTests::processNextBatch_whenCallbackArrivesAfterCancellation_shouldDiscardLateResult() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    addInbox(fixture.memory);
    fixture.modelClient.replies.append({true, QStringLiteral("[]"), {}, {}});
    fixture.modelClient.deferCallbacks = true;
    DaydreamSleepAdapter adapter = fixture.daydreamAdapter();
    CancellationSource cancellation;
    const CancellationToken token = cancellation.token();
    StagingSession staging{QStringLiteral("sleep-3"), token.generation()};
    bool completed = false;
    adapter.consolidateAsync({kProfileId, staging.sessionId, 0, 1}, staging,
                             token, [&](Result<DaydreamChangeSet, DomainError> result) {
        QVERIFY(!result.isOk());
        QCOMPARE(result.error().code, QStringLiteral("SLEEP_CANCELLED"));
        completed = true;
    });
    QVERIFY(!completed);
    QCOMPARE(fixture.modelClient.selectedRouteIds,
             QList<QString>{QString::number(
                 static_cast<int>(ModelRole::Daydream))});
    cancellation.cancel();
    fixture.modelClient.finishNext();
    QVERIFY(completed);
    QCOMPARE(fixture.memory.preparedSleepChangeCount(staging.sessionId), 0);
}

void SleepCycleTests::runNextDaydreamBatch_whenModelDecisionIsRequired_shouldRequestDaydreamRoleAndApplyDecision() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FakeModelClient modelClient;
    AIBrain brain(
        &modelClient,
        {roleConfig(ModelRole::Dialogue), roleConfig(ModelRole::Daydream)});
    QVERIFY(brain.initializeStorage({
        directory.filePath(QStringLiteral("brain-memory.db")),
        directory.filePath(QStringLiteral("brain-memory.json"))}).isOk());
    const MemoryEntry source = addInbox(*brain.memoryStore());
    modelClient.replies.append(
        {true, discardDecisionJson(source.id), {}, {}});
    QSignalSpy finished(&brain, &AIBrain::daydreamFinished);

    TestIdentityState::runPreparedDaydreamBatch(brain);

    QCOMPARE(modelClient.selectedRouteIds,
             QList<QString>{QString::number(
                 static_cast<int>(ModelRole::Daydream))});
    QCOMPARE(finished.count(), 1);
    QVERIFY(!brain.memoryStore()->findById(source.id));
}

void SleepCycleTests::runNextDaydreamBatch_whenDaydreamRoutesFail_shouldUseBoundedHardcodedFallback() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    FakeModelClient modelClient;
    AIBrain brain(
        &modelClient,
        {roleConfig(ModelRole::Dialogue), roleConfig(ModelRole::Daydream)});
    QVERIFY(brain.initializeStorage({
        directory.filePath(QStringLiteral("brain-fallback.db")),
        directory.filePath(QStringLiteral("brain-fallback.json"))}).isOk());
    const MemoryEntry source = addInbox(*brain.memoryStore());
    modelClient.replies.append(
        {false, {}, QStringLiteral("all Daydream routes failed"), {}});
    QSignalSpy finished(&brain, &AIBrain::daydreamFinished);

    TestIdentityState::runPreparedDaydreamBatch(brain);

    QCOMPARE(modelClient.selectedRouteIds,
             QList<QString>{QString::number(
                 static_cast<int>(ModelRole::Daydream))});
    QCOMPARE(finished.count(), 1);
    const QJsonObject summary = finished.first().first().toJsonObject();
    QCOMPARE(summary.value(QStringLiteral("fallbackBatches")).toInt(), 1);
    QVERIFY(!brain.memoryStore()->findById(source.id));
    QCOMPARE(brain.memoryStore()->all().size(), 1);
    QVERIFY(brain.memoryStore()->all().first().partition
            != QLatin1String("hippocampus"));
}

void SleepCycleTests::buildChangeSet_whenDecisionsAreValid_shouldReturnDeterministicChangesWithoutMutatingStore() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    const MemoryEntry source = addInbox(fixture.memory);
    DaydreamConsolidator consolidator(fixture.memory);
    const auto snapshot = consolidator.createSnapshot(1);
    const auto decisions = DaydreamConsolidator::hardcodedDecisions(snapshot.items);
    const auto first = consolidator.buildChangeSet(snapshot, decisions);
    const auto second = consolidator.buildChangeSet(snapshot, decisions);
    QVERIFY(first.isOk());
    QVERIFY(second.isOk());
    QCOMPARE(first.value().changeSetId, second.value().changeSetId);
    QVERIFY(fixture.memory.findById(source.id));
}

void SleepCycleTests::applyChangeSet_whenCommitIsDurable_shouldMaterializeChangesOnce() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    addInbox(fixture.memory);
    DaydreamConsolidator consolidator(fixture.memory);
    const auto snapshot = consolidator.createSnapshot(1);
    const auto changeSet = consolidator.buildChangeSet(
        snapshot, DaydreamConsolidator::hardcodedDecisions(snapshot.items));
    QVERIFY(changeSet.isOk());
    const auto first = consolidator.applyChangeSet(changeSet.value());
    const int countAfterFirst = fixture.memory.all().size();
    const auto second = consolidator.applyChangeSet(changeSet.value());
    QVERIFY(first.committed);
    QVERIFY(second.committed);
    QCOMPARE(fixture.memory.all().size(), countAfterFirst);
}

void SleepCycleTests::finalizeSession_whenLegacySecondPrecisionChangeSetIsLoaded_shouldRecover() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    const QDateTime legacyTimestamp = QDateTime::fromString(
        QDateTime::currentDateTimeUtc().addSecs(-60).toString(Qt::ISODate),
        Qt::ISODate);
    const MemoryEntry source = addInboxAt(
        fixture.memory, legacyTimestamp, QStringLiteral("owner likes tea"));
    DaydreamConsolidator consolidator(fixture.memory);
    const auto snapshot = consolidator.createSnapshot(1);
    const auto changeSet = consolidator.buildChangeSet(
        snapshot, DaydreamConsolidator::hardcodedDecisions(snapshot.items));
    QVERIFY(changeSet.isOk());

    const QJsonObject legacy = legacySecondPrecisionChangeSet(changeSet.value());
    const auto parsed = DaydreamChangeSet::fromJson(legacy);
    QVERIFY(parsed.isOk());
    QCOMPARE(parsed.value().changeSetId,
             legacy.value(QStringLiteral("changeSetId")).toString());

    const QString sessionId = QStringLiteral("legacy-second-precision");
    StagedMemoryChange staged;
    staged.sessionId = sessionId;
    staged.changeId = parsed.value().changeSetId;
    staged.targetType = QStringLiteral("daydream_change_set");
    staged.operation = QStringLiteral("apply");
    staged.payload = legacy;
    QVERIFY(fixture.memory.stageSleepChange(staged));
    DaydreamSleepAdapter adapter = fixture.daydreamAdapter();

    const auto finalized = adapter.finalizeSession(sessionId);

    QVERIFY(finalized.isOk());
    QVERIFY(!fixture.memory.findById(source.id));
    QCOMPARE(adapter.preparedChangeCount(sessionId), 0);
}

void SleepCycleTests::applyDecisions_whenCalledByLegacyDaydream_shouldDelegateAndPreserveExistingBehavior() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    addInbox(fixture.memory);
    DaydreamConsolidator consolidator(fixture.memory);
    const auto snapshot = consolidator.createSnapshot(1);
    const auto stats = consolidator.applyDecisions(
        snapshot, DaydreamConsolidator::hardcodedDecisions(snapshot.items));
    QVERIFY(stats.committed);
    QCOMPARE(stats.scanned, 1);
}

void SleepCycleTests::composeAsync_whenBedtimeContextIsValid_shouldStageOneEncryptedDiaryForLocalDate() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    fixture.modelClient.replies.append({true, diaryJson(), {}, {}});
    DiaryService service = fixture.diaryService();
    StagingSession staging{QStringLiteral("sleep-diary-1"), 1};
    CancellationSource cancellation;
    bool completed = false;
    service.composeAsync(diaryRequest(staging.sessionId), staging,
                         cancellation.token(), [&](Result<QString, DomainError> result) {
        QVERIFY(result.isOk());
        completed = true;
    });
    QVERIFY(completed);
    QCOMPARE(fixture.privateRepository.preparedDiaryCount(staging.sessionId), 1);
    QVERIFY(!fixture.privateRepository.stagedDiaryCiphertext(staging.sessionId).contains(
        QStringLiteral("今天").toUtf8()));
}

void SleepCycleTests::composeAsync_whenDateAlreadyCommitted_shouldReturnExistingEntryId() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    fixture.modelClient.replies.append({true, diaryJson(), {}, {}});
    DiaryService service = fixture.diaryService();
    CancellationSource cancellation;
    StagingSession first{QStringLiteral("sleep-diary-2"), 1};
    QString firstId;
    service.composeAsync(diaryRequest(first.sessionId), first, cancellation.token(),
                         [&](Result<QString, DomainError> result) {
        QVERIFY(result.isOk());
        firstId = result.value();
    });
    QVERIFY(service.finalizeSession(first.sessionId).isOk());
    StagingSession second{QStringLiteral("sleep-diary-3"), 1};
    QString secondId;
    service.composeAsync(diaryRequest(second.sessionId), second, cancellation.token(),
                         [&](Result<QString, DomainError> result) {
        QVERIFY(result.isOk());
        secondId = result.value();
    });
    QCOMPARE(secondId, firstId);
    QCOMPARE(fixture.privateRepository.preparedDiaryCount(second.sessionId), 0);
}

void SleepCycleTests::composeAsync_whenModelOutputIsEmptyAfterRepair_shouldKeepRetryablePendingState() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    fixture.modelClient.replies.append({true, QString(), {}, {}});
    fixture.modelClient.replies.append({true, QString(), {}, {}});
    DiaryService service = fixture.diaryService();
    StagingSession staging{QStringLiteral("sleep-diary-empty"), 1};
    CancellationSource cancellation;
    QString errorCode;
    service.composeAsync(diaryRequest(staging.sessionId), staging,
                         cancellation.token(), [&](Result<QString, DomainError> result) {
        QVERIFY(!result.isOk());
        errorCode = result.error().code;
    });
    QCOMPARE(errorCode, QStringLiteral("MODEL_OUTPUT_INVALID"));
    QCOMPARE(fixture.privateRepository.preparedDiaryCount(staging.sessionId), 0);
}

void SleepCycleTests::composeAsync_whenKeychainUnavailable_shouldReturnPrivateStoreUnavailableWithoutPlaintextFallback() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    fixture.keys.available = false;
    fixture.modelClient.replies.append({true, diaryJson(), {}, {}});
    DiaryService service = fixture.diaryService();
    StagingSession staging{QStringLiteral("sleep-no-key"), 1};
    CancellationSource cancellation;
    QString errorCode;
    service.composeAsync(diaryRequest(staging.sessionId), staging,
                         cancellation.token(), [&](Result<QString, DomainError> result) {
        QVERIFY(!result.isOk());
        errorCode = result.error().code;
    });
    QCOMPARE(errorCode, QStringLiteral("PRIVATE_STORE_UNAVAILABLE"));
    QCOMPARE(fixture.privateRepository.preparedDiaryCount(staging.sessionId), 0);
}

void SleepCycleTests::privateServices_whenBuildCapabilityIsUnavailable_shouldReturnPrivateStoreUnavailableWithoutPlaintextFallback() {
#if DESKTOP_PET_HAS_PRIVATE_REFLECTION
    QVERIFY(privateReflectionBuildAvailable());
#else
    QVERIFY(!privateReflectionBuildAvailable());
    QtKeychainPrivateKeyProvider provider;
    const auto result = provider.loadOrCreate(kProfileId);
    QVERIFY(!result.isOk());
    QCOMPARE(result.error().code, QStringLiteral("PRIVATE_STORE_UNAVAILABLE"));
#endif
}

void SleepCycleTests::readForSelf_whenDiaryRoleHasEntryScope_shouldDecryptSelectedEntry() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    fixture.modelClient.replies.append({true, diaryJson(), {}, {}});
    DiaryService service = fixture.diaryService(ModelRole::Diary);
    StagingSession staging{QStringLiteral("sleep-read-self"), 1};
    CancellationSource cancellation;
    QString entryId;
    service.composeAsync(diaryRequest(staging.sessionId), staging, cancellation.token(),
                         [&](Result<QString, DomainError> result) { entryId = result.value(); });
    QVERIFY(service.finalizeSession(staging.sessionId).isOk());
    const auto result = service.readForSelf(entryId);
    QVERIFY(result.isOk());
    QCOMPARE(result.value().body, QStringLiteral("今天和主人聊了喝茶的事。"));
}

void SleepCycleTests::readForSelf_whenDialogueScopeRequestsEntry_shouldReturnScopeDenied() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    DiaryService service = fixture.diaryService(ModelRole::Dialogue);
    const auto result = service.readForSelf(
        QStringLiteral("22222222-2222-4222-8222-222222222222"));
    QVERIFY(!result.isOk());
    QCOMPARE(result.error().code, QStringLiteral("CONTEXT_SCOPE_DENIED"));
}

void SleepCycleTests::readForOwner_whenAuthAndProfileMatch_shouldDecryptSingleEntry() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    fixture.modelClient.replies.append({true, diaryJson(), {}, {}});
    DiaryService service = fixture.diaryService();
    StagingSession staging{QStringLiteral("sleep-read-owner"), 1};
    CancellationSource cancellation;
    QString entryId;
    service.composeAsync(diaryRequest(staging.sessionId), staging, cancellation.token(),
                         [&](Result<QString, DomainError> result) { entryId = result.value(); });
    QVERIFY(service.finalizeSession(staging.sessionId).isOk());
    const auto result = service.readForOwner(
        entryId, OwnerAuthContext{kProfileId, true});
    QVERIFY(result.isOk());
}

void SleepCycleTests::readForOwner_whenAadOrCiphertextIsModified_shouldRejectAuthentication() {
    {
        ReflectionFixture fixture;
        QVERIFY(fixture.open());
        fixture.modelClient.replies.append({true, diaryJson(), {}, {}});
        DiaryService service = fixture.diaryService();
        StagingSession staging{QStringLiteral("sleep-tamper-ciphertext"), 1};
        CancellationSource cancellation;
        QString entryId;
        service.composeAsync(
            diaryRequest(staging.sessionId), staging, cancellation.token(),
            [&](Result<QString, DomainError> result) { entryId = result.value(); });
        QVERIFY(service.finalizeSession(staging.sessionId).isOk());
        QVERIFY(replaceDiaryCiphertext(fixture.privateRepository, entryId,
                                       QByteArrayLiteral("modified")));
        const auto result = service.readForOwner(
            entryId, OwnerAuthContext{kProfileId, true});
        QVERIFY(!result.isOk());
        QCOMPARE(result.error().code, QStringLiteral("PRIVATE_AUTH_FAILED"));
    }
    {
        ReflectionFixture fixture;
        QVERIFY(fixture.open());
        fixture.modelClient.replies.append({true, diaryJson(), {}, {}});
        DiaryService service = fixture.diaryService();
        StagingSession staging{QStringLiteral("sleep-tamper-aad"), 1};
        CancellationSource cancellation;
        QString entryId;
        service.composeAsync(
            diaryRequest(staging.sessionId), staging, cancellation.token(),
            [&](Result<QString, DomainError> result) { entryId = result.value(); });
        QVERIFY(service.finalizeSession(staging.sessionId).isOk());
        QVERIFY(replaceDiaryProfile(
            fixture.privateRepository, entryId,
            QStringLiteral("22222222-2222-4222-8222-222222222222")));
        const auto result = service.readForOwner(
            entryId, OwnerAuthContext{kProfileId, true});
        QVERIFY(!result.isOk());
        QCOMPARE(result.error().code, QStringLiteral("PRIVATE_AUTH_FAILED"));
    }
}

void SleepCycleTests::tryStart_whenBedtimeIdleAndNoDueTask_shouldCreateDurablePendingSession() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    SleepPolicy policy;
    policy.bedtime = QTime(0, 0);
    SleepCycleHooks hooks;
    hooks.isBrainBusy = [] { return false; };
    hooks.hasTaskDueBefore = [](const QDateTime&) { return false; };
    hooks.userIdleSeconds = [] { return 3600; };
    hooks.sourceCutoffSequence = [] { return 12; };
    SleepCycleCoordinator coordinator(
        kProfileId, policy, &fixture.sleepSessions, nullptr, nullptr,
        &fixture.privateRepository, nullptr, nullptr, hooks);
    const auto result = coordinator.tryStart({
        SleepTriggerType::Bedtime, 3600, QDateTime::currentDateTime(), kProfileId});
    QVERIFY(result.isOk());
    const auto session = fixture.sleepSessions.find(result.value());
    QVERIFY(session.isOk());
    QVERIFY(session.value().has_value());
    QCOMPARE(session.value()->decision, SleepDecision::Pending);
}

void SleepCycleTests::tryStart_whenBrainBusyOrTaskDueSoon_shouldNotStart() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    SleepPolicy policy;
    policy.bedtime = QTime(0, 0);
    SleepCycleHooks hooks;
    hooks.isBrainBusy = [] { return true; };
    hooks.hasTaskDueBefore = [](const QDateTime&) { return false; };
    hooks.userIdleSeconds = [] { return 3600; };
    hooks.sourceCutoffSequence = [] { return 12; };
    SleepCycleCoordinator coordinator(
        kProfileId, policy, &fixture.sleepSessions, nullptr, nullptr,
        &fixture.privateRepository, nullptr, nullptr, hooks);
    const auto result = coordinator.tryStart({
        SleepTriggerType::Bedtime, 3600, QDateTime::currentDateTime(), kProfileId});
    QVERIFY(!result.isOk());
    QCOMPARE(result.error().code, QStringLiteral("SLEEP_NOT_READY"));
}

void SleepCycleTests::tryStart_whenDiaryAlreadyCommitted_shouldSkipBedtimeButAllowManual() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    fixture.modelClient.replies.append({true, diaryJson(), {}, {}});
    DiaryService diary = fixture.diaryService();
    StagingSession staging{QStringLiteral("sleep-existing-diary"), 1};
    CancellationSource cancellation;
    bool composed = false;
    diary.composeAsync(
        diaryRequest(staging.sessionId), staging, cancellation.token(),
        [&](Result<QString, DomainError> result) { composed = result.isOk(); });
    QVERIFY(composed);
    QVERIFY(diary.finalizeSession(staging.sessionId).isOk());

    SleepPolicy policy;
    policy.bedtime = QTime(0, 0);
    SleepCycleHooks hooks;
    hooks.isBrainBusy = [] { return false; };
    hooks.hasTaskDueBefore = [](const QDateTime&) { return false; };
    hooks.userIdleSeconds = [] { return 3600; };
    hooks.sourceCutoffSequence = [] { return 12; };
    SleepCycleCoordinator coordinator(
        kProfileId, policy, &fixture.sleepSessions, nullptr, &diary,
        &fixture.privateRepository, nullptr, nullptr, hooks);
    const QDateTime bedtimeNow(kLocalDate, QTime(23, 0));
    const auto bedtime = coordinator.tryStart({
        SleepTriggerType::Bedtime, 3600, bedtimeNow, kProfileId});
    QVERIFY(!bedtime.isOk());
    QCOMPARE(bedtime.error().code, QStringLiteral("SLEEP_NOT_READY"));

    const auto manual = coordinator.tryStart({
        SleepTriggerType::Manual, 0, bedtimeNow, kProfileId});
    QVERIFY(manual.isOk());
}

void SleepCycleTests::tryStart_whenAllParticipantsPrepared_shouldPersistCommitThenFinalizeAllStores() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    const MemoryEntry source = addInbox(fixture.memory);
    queueSleepReplies(fixture);
    DaydreamSleepAdapter adapter = fixture.daydreamAdapter();
    DiaryService diary = fixture.diaryService();
    SleepPolicy policy;
    policy.bedtime = QTime(0, 0);
    SleepCycleHooks hooks;
    hooks.isBrainBusy = [] { return false; };
    hooks.hasTaskDueBefore = [](const QDateTime&) { return false; };
    hooks.userIdleSeconds = [] { return 3600; };
    hooks.sourceCutoffSequence = [] { return 12; };
    SleepCycleCoordinator coordinator(
        kProfileId, policy, &fixture.sleepSessions, &adapter, &diary,
        &fixture.privateRepository, nullptr, nullptr, hooks);
    const auto result = coordinator.tryStart({
        SleepTriggerType::Bedtime, 3600, QDateTime::currentDateTime(), kProfileId});
    QVERIFY(result.isOk());
    const auto session = fixture.sleepSessions.find(result.value());
    QVERIFY(session.isOk() && session.value().has_value());
    QCOMPARE(session.value()->decision, SleepDecision::Commit);
    QCOMPARE(session.value()->state, SleepSessionState::Completed);
    QCOMPARE(fixture.privateRepository.diaryCount(kProfileId), 1);
    QVERIFY(!fixture.memory.findById(source.id));
    QCOMPARE(fixture.memory.all().size(), 1);
    QVERIFY(fixture.memory.all().first().partition != QLatin1String("hippocampus"));
}

void SleepCycleTests::tryStart_whenRestartFindsCommittedSession_shouldIdempotentlyFinishFinalize() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    const MemoryEntry source = addInbox(fixture.memory);
    const QString sessionId = QStringLiteral("restart-commit");
    QVERIFY(fixture.sleepSessions.createPending(
        SleepSessionRecord{sessionId, kProfileId, 0}).isOk());
    DaydreamSleepAdapter adapter = fixture.daydreamAdapter();
    DiaryService diary = fixture.diaryService();
    QVERIFY(prepareSleepParticipants(fixture, sessionId, adapter, diary));
    QVERIFY(fixture.sleepSessions.decideCommit(sessionId).isOk());
    SleepCycleCoordinator coordinator(
        kProfileId, SleepPolicy{}, &fixture.sleepSessions, &adapter, &diary,
        &fixture.privateRepository, nullptr, nullptr, {});
    QVERIFY(coordinator.recoverIncomplete().isOk());
    QVERIFY(coordinator.recoverIncomplete().isOk());
    const auto session = fixture.sleepSessions.find(sessionId);
    QCOMPARE(session.value()->state, SleepSessionState::Completed);
    QVERIFY(!fixture.memory.findById(source.id));
    QCOMPARE(fixture.privateRepository.diaryCount(kProfileId), 1);
}

void SleepCycleTests::cancel_whenDecisionPending_shouldAbortAllStagingAndPreserveFormalState() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    const MemoryEntry source = addInbox(fixture.memory);
    const QString sessionId = QStringLiteral("cancel-pending");
    QVERIFY(fixture.sleepSessions.createPending(
        SleepSessionRecord{sessionId, kProfileId, 0}).isOk());
    DaydreamSleepAdapter adapter = fixture.daydreamAdapter();
    DiaryService diary = fixture.diaryService();
    QVERIFY(prepareSleepParticipants(fixture, sessionId, adapter, diary));
    SleepCycleCoordinator coordinator(
        kProfileId, SleepPolicy{}, &fixture.sleepSessions, &adapter, &diary,
        &fixture.privateRepository, nullptr, nullptr, {});
    QVERIFY(coordinator.cancel(sessionId, SleepCancelReason::UserInteraction).isOk());
    QVERIFY(fixture.memory.findById(source.id));
    QCOMPARE(fixture.sleepSessions.find(sessionId).value()->decision,
             SleepDecision::Abort);
    QCOMPARE(adapter.preparedChangeCount(sessionId), 0);
    QCOMPARE(fixture.privateRepository.preparedDiaryCount(sessionId), 0);
    QCOMPARE(fixture.privateRepository.diaryCount(kProfileId), 0);
}

void SleepCycleTests::cancel_whenDecisionCommitted_shouldKeepCommitAndFinishFinalize() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    const MemoryEntry source = addInbox(fixture.memory);
    const QString sessionId = QStringLiteral("cancel-commit");
    QVERIFY(fixture.sleepSessions.createPending(
        SleepSessionRecord{sessionId, kProfileId, 0}).isOk());
    DaydreamSleepAdapter adapter = fixture.daydreamAdapter();
    DiaryService diary = fixture.diaryService();
    QVERIFY(prepareSleepParticipants(fixture, sessionId, adapter, diary));
    QVERIFY(fixture.sleepSessions.decideCommit(sessionId).isOk());
    SleepCycleCoordinator coordinator(
        kProfileId, SleepPolicy{}, &fixture.sleepSessions, &adapter, &diary,
        &fixture.privateRepository, nullptr, nullptr, {});
    QVERIFY(coordinator.cancel(sessionId, SleepCancelReason::UserInteraction).isOk());
    const auto session = fixture.sleepSessions.find(sessionId);
    QCOMPARE(session.value()->decision, SleepDecision::Commit);
    QCOMPARE(session.value()->state, SleepSessionState::Completed);
    QVERIFY(!fixture.memory.findById(source.id));
    QCOMPARE(fixture.privateRepository.diaryCount(kProfileId), 1);
}

void SleepCycleTests::recoverIncomplete_whenCommittedSessionExists_shouldFinalizeBeforePublishingCapability() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    const MemoryEntry source = addInbox(fixture.memory);
    const QString sessionId = QStringLiteral("recover-before-start");
    QVERIFY(fixture.sleepSessions.createPending(
        SleepSessionRecord{sessionId, kProfileId, 0}).isOk());
    DaydreamSleepAdapter adapter = fixture.daydreamAdapter();
    DiaryService diary = fixture.diaryService();
    QVERIFY(prepareSleepParticipants(fixture, sessionId, adapter, diary));
    QVERIFY(fixture.sleepSessions.decideCommit(sessionId).isOk());
    bool published = false;
    SleepCycleHooks hooks;
    hooks.publishCapability = [&published](bool value) { published = value; };
    SleepCycleCoordinator coordinator(
        kProfileId, SleepPolicy{}, &fixture.sleepSessions, &adapter, &diary,
        &fixture.privateRepository, nullptr, nullptr, hooks);
    coordinator.start();
    QVERIFY(published);
    QCOMPARE(fixture.sleepSessions.find(sessionId).value()->state,
             SleepSessionState::Completed);
    QVERIFY(!fixture.memory.findById(source.id));
    QCOMPARE(fixture.privateRepository.diaryCount(kProfileId), 1);

    const QString missingStageSession = QStringLiteral("recover-missing-stage");
    QVERIFY(fixture.sleepSessions.createPending(
        SleepSessionRecord{missingStageSession, kProfileId, 0}).isOk());
    QVERIFY(fixture.sleepSessions.decideCommit(missingStageSession).isOk());
    QVERIFY(!coordinator.recoverIncomplete().isOk());
    QCOMPARE(fixture.sleepSessions.find(missingStageSession).value()->state,
             SleepSessionState::Committing);
}

void SleepCycleTests::start_whenCoordinatorTakesOwnership_shouldDisableLegacyDaydreamTimer() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    AIBrain brain;
    SleepCycleCoordinator coordinator(
        kProfileId, SleepPolicy{}, &fixture.sleepSessions, nullptr, nullptr,
        &fixture.privateRepository, &brain, nullptr, {});
    coordinator.start();
    QVERIFY(brain.isExternalSleepCoordinatorEnabled());
    coordinator.stop();
    QVERIFY(!brain.isExternalSleepCoordinatorEnabled());
}

void SleepCycleTests::stop_whenCallbacksAreLate_shouldInvalidateGenerationAndPerformNoWrites() {
    ReflectionFixture fixture;
    QVERIFY(fixture.open());
    const MemoryEntry source = addInbox(fixture.memory);
    fixture.modelClient.replies.append(
        {false, {}, QStringLiteral("late model failure"), {}});
    fixture.modelClient.deferCallbacks = true;
    DaydreamSleepAdapter adapter = fixture.daydreamAdapter();
    DiaryService diary = fixture.diaryService();
    SleepPolicy policy;
    policy.bedtime = QTime(0, 0);
    SleepCycleHooks hooks;
    hooks.isBrainBusy = [] { return false; };
    hooks.hasTaskDueBefore = [](const QDateTime&) { return false; };
    hooks.userIdleSeconds = [] { return 3600; };
    hooks.sourceCutoffSequence = [] { return 12; };
    SleepCycleCoordinator coordinator(
        kProfileId, policy, &fixture.sleepSessions, &adapter, &diary,
        &fixture.privateRepository, nullptr, nullptr, hooks);
    const quint64 generation = coordinator.generation();
    coordinator.start();
    const auto started = coordinator.tryStart({
        SleepTriggerType::Bedtime, 3600, QDateTime::currentDateTime(), kProfileId});
    QVERIFY(started.isOk());
    QCOMPARE(fixture.modelClient.pending.size(), 1);
    coordinator.stop();
    fixture.modelClient.finishNext();
    QVERIFY(coordinator.generation() > generation);
    const auto session = fixture.sleepSessions.find(started.value());
    QVERIFY(session.isOk() && session.value().has_value());
    QCOMPARE(session.value()->state, SleepSessionState::RolledBack);
    QVERIFY(fixture.memory.findById(source.id));
    QCOMPARE(fixture.privateRepository.diaryCount(kProfileId), 0);
}

void SleepCycleTests::getSleepPolicy_whenConfigured_shouldReturnSanitizedPolicy() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QJsonObject sleepPolicy{
        {QStringLiteral("enabled"), true},
        {QStringLiteral("bedtime"), QStringLiteral("22:15")},
        {QStringLiteral("minimumIdleSeconds"), 900},
        {QStringLiteral("dueSoonThresholdSeconds"), 300},
        {QStringLiteral("maxItemsPerSession"), 16},
        {QStringLiteral("retryBackoffSeconds"), 120},
        {QStringLiteral("tickIntervalSeconds"), 30}
    };
    const QJsonObject profile{
        {QStringLiteral("enabled"), true},
        {QStringLiteral("sleepPolicy"), sleepPolicy}
    };
    const QJsonObject root{{QStringLiteral("aiSettings"), QJsonObject{
        {QStringLiteral("activeProfile"), QStringLiteral("reflection")},
        {QStringLiteral("profiles"), QJsonObject{
             {QStringLiteral("reflection"), profile}}}
    }}};
    QVERIFY(ConfigManager::instance().loadConfig(writeConfig(directory, root)));
    const SleepPolicy& policy = ConfigManager::instance().getSleepPolicy();
    QCOMPARE(policy.bedtime, QTime(22, 15));
    QCOMPARE(policy.minimumIdleSeconds, 900);
    QCOMPARE(policy.maxItemsPerSession, 16);
}

void SleepCycleTests::getSleepPolicy_whenMissingOrInvalid_shouldUseSafeDefaults() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QJsonObject sleepPolicy{
        {QStringLiteral("bedtime"), QStringLiteral("invalid")},
        {QStringLiteral("minimumIdleSeconds"), -1},
        {QStringLiteral("maxItemsPerSession"), 9999}
    };
    const QJsonObject profile{{QStringLiteral("sleepPolicy"), sleepPolicy}};
    const QJsonObject root{{QStringLiteral("aiSettings"), QJsonObject{
        {QStringLiteral("activeProfile"), QStringLiteral("reflection")},
        {QStringLiteral("profiles"), QJsonObject{
             {QStringLiteral("reflection"), profile}}}
    }}};
    QVERIFY(ConfigManager::instance().loadConfig(writeConfig(directory, root)));
    const SleepPolicy& policy = ConfigManager::instance().getSleepPolicy();
    QCOMPARE(policy.bedtime, QTime(23, 30));
    QCOMPARE(policy.minimumIdleSeconds, 600);
    QCOMPARE(policy.maxItemsPerSession, 32);
}

QTEST_MAIN(SleepCycleTests)
#include "test_sleep_cycle.moc"
