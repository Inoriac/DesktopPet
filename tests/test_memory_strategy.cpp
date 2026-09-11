//
// Memory extractor / policy / retriever / relation tests
//

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <filesystem>
#include <limits>

#include "memory/memory_extractor.h"
#include "memory/memory_policy.h"
#include "memory/memory_relation.h"
#include "memory/memory_relation_graph.h"
#include "memory/memory_retriever.h"
#include "memory/memory_store.h"
#include "memory/working_memory_cache.h"
#include "memory/noop_embedding_index.h"
#include "memory/partition_policy.h"
#include "memory/sqlite_embedding_index.h"
#include "memory/hnsw_embedding_index.h"
#include "memory/memory_index_worker.h"
#include "memory/semantic_index_service.h"
#include "memory/sqlite_memory_repository.h"
#include "memory/model_downloader.h"
#include "memory/daydream_consolidator.h"
#include "scheduler/daydream_trigger_policy.h"
#include "skill/skill_store.h"
#include "tools/memory_tools.h"

#ifdef DESKTOP_PET_HAS_ORT
#include "memory/onnx_embedding_provider.h"
#include <cmath>
#endif

class TestMemoryStrategy : public QObject {
    Q_OBJECT

private slots:
    void testExtractorCreatesPreferenceFromLikeStatement();
    void testExtractorCreatesSemanticFromRememberStatement();
    void testExtractorCreatesForgetCandidate();
    void testExtractorCreatesDaydreamImpressionForSelfDisclosure();
    void testExtractorRejectsUnsafeDaydreamImpressions();
    void testPolicyWritesAndSkipsDuplicate();
    void testPolicyRejectsSensitiveMemory();
    void testPolicyMarksMatchedMemoryDeleted();
    void stageCandidates_whenWriteSupersedesAndForget_shouldKeepGuiCacheAheadOfPersistence();
    void testStoreUpdateEntryByIdPersists();
    void testStoreDoesNotMutateWhenPersistenceFails();
    void testRepositoryClearRollsBackOnFailure();
    void testSkillStoreDoesNotMutateWhenPersistenceFails();
    void testStoreUpdateStatusByIdIsExact();
    void testRetrieverRanksKeywordAndPreferredType();
    void testRetrieverFiltersSensitiveByDefault();
    void testRetrieverFormatsContextLines();
    void testPolicyCreatesSupersedes();
    void testPolicyCreatesConflictsWith();
    void testPolicyCreatesRelatedByTags();
    void testPolicyCreatesMentionedWith();
    void testPolicyFirstOfScopeImportanceBoost();
    void testRetrieverDecayCurve();
    void testRetrieverEmotionBoost();
    void testRetrieverReinforcement();
    void testRetrieverReinforcementPersists();
    void stageReinforcement_whenIdsExist_shouldUpdateMemoryAndReturnOneBatch();
    void stageReinforcement_whenIdsRepeatOrAreMissing_shouldUpdateEachKnownEntryOnce();
    void testRetrieverGraphExpansion();
    void testMemoryOrganizeDryRun();
    void testMemoryOrganizeExpiresWithoutDeleting();
    void testMemoryOrganizeArchivesOperationalEvents();
    void testMemoryOrganizeMergesDuplicates();
    void testMemoryOrganizeSkipsSensitiveOutput();
    void testWorkingMemoryCacheTtl();
    void testWorkingMemoryCacheCapacity();
    void testWorkingMemoryCacheConsolidation();
    void testWorkingMemoryRetrieverIntegration();
    void testNoopEmbeddingIndexDoesNotAffectRetrieval();
    void testPartitionMappingForAllTypes();
    void testAdaptiveDecayRetentionAndForgetDays();
    void testPartitionPersistedAndBackfilled();
    void testLegacySchemaWithoutPartitionMigratesBeforeIndexCreation();
    void testForgettingSweepExpiresStaleAndSparesImportant();
    void testSqliteEmbeddingIndexSearch();
    void testHnswEmbeddingIndexSearchAndPersistence();
    void testMemoryIndexWorkerProcessesOutbox();
    void testMemoryIndexWorkerDurableCompletion();
    void testMemoryIndexWorkerRetriesFailedSave();
    void testMemoryIndexWorkerDeleteSurvivesRestart();
    void testHnswRejectsMismatchedFiles();
    void testHnswUpdatesLabelsAndRebuildsFromAuthoritativeVectors();
    void testIndexWorkerAutomaticRecovery_data();
    void testIndexWorkerAutomaticRecovery();
    void testHnswRebuildFiltersInvalidRows();
    void testHnswRebuildFailureIsRetryable_data();
    void testHnswRebuildFailureIsRetryable();
    void testIndexWorkerRecoveryFailureKeepsJobPending();
    void testHnswReinsertAndInvalidVectors();
    void testIndexJobsReconcileCurrentState();
    void testIndexJobsRespectModelBinding();
    void testIndexJobsRejectChangesDuringEmbedding();
    void testIndexJobsPersistBackoff();
    void testSemanticIndexServiceLifecycle();
    void testModelDownloaderLocalMirror();
    void testTransactionRollbackRevertsWrites();
    void testTransactionCommitRetainsWrites();
    void testNestedTransactionsKeepOutboxAtomic();
    void testNestedOutboxFailurePreservesOuterTransaction();
    void testRepositoryTransactionsRespectExternalTransaction();
    void testTransactionRollbackRevertsRelationGraph();
    void testTransactionRollbackRevertsTagCooccurrence();
    void testDaydreamDrainUpgradesAndClearsHippocampus();
    void testDaydreamOutboxFailureRollsBackBatch();
    void testDaydreamUpdatesTagCooccurrenceGraph();
    void testDaydreamUpdateRecordsTagCooccurrence();
    void testDaydreamTagCooccurrenceAccumulates();
    void testDaydreamDrainDiscardsLowValue();
    void testDaydreamDrainSparesOtherPartitions();
    void testStoreKeyPersistsRoundtrip();
    void testDaydreamDrainUpgradesViaPersistedMentionCount();
    void testDaydreamFallbackUpgradesHighImportance();
    void testDaydreamFallbackRoutesPreferenceKeyword();
    void testDaydreamFallbackDeduplicatesBatch();
    void testDaydreamDiscardsLegacyAssistantInbox();
    void testDaydreamSessionLimitLeavesRemainder();
    void testDaydreamRejectsStaleSnapshotAtomically();
    void testDaydreamRejectsStaleUpdateTarget();
    void testDaydreamSnapshotDoesNotConsumeNewInboxItems();
    void testDaydreamParsesValidatedLlmDecisions();
    void testDaydreamParsesRelationsProposalsFromObjectRoot();
    void testDaydreamBuildChangeSetValidatesProposals();
    void testDaydreamChangeSetProposalsHashCompatWithLegacyPayload();
    void testDaydreamTriggerPolicyAllConditions();
    void testDaydreamTriggerPolicyNegativeCases();
    void testDaydreamTriggerPolicyNoDueTodoNonBlocking();
    void testDaydreamTriggerPolicyContinuation();
    void testDaydreamTriggerPolicyUsesRuntimeConfig();
#ifdef DESKTOP_PET_HAS_ORT
    void testOnnxEmbeddingProviderLoadsAndEmbeds();
#endif

private:
    void setupStoreWithDb(MemoryStore& store, const QTemporaryDir& dir);
};

void TestMemoryStrategy::setupStoreWithDb(MemoryStore& store, const QTemporaryDir& dir) {
    store.setStoragePath(dir.filePath(QStringLiteral("memory.json")));
    store.setDatabasePath(dir.filePath(QStringLiteral("memory.db")));
    QVERIFY(store.load());
}

void TestMemoryStrategy::testExtractorCreatesPreferenceFromLikeStatement() {
    MemoryExtractor extractor;
    const QList<MemoryCandidate> candidates = extractor.extractFromUserInput(QStringLiteral("我喜欢 Java"), QStringLiteral("user_request"));

    QCOMPARE(candidates.size(), 1);
    QCOMPARE(candidates.first().operation, MemoryCandidateOperation::Write);
    QCOMPARE(candidates.first().entry.type, MemoryType::Preference);
    QCOMPARE(candidates.first().entry.privacyLevel, PrivacyLevel::Personal);
    QVERIFY(candidates.first().entry.summary.contains(QStringLiteral("用户喜欢")));
    QVERIFY(candidates.first().entry.summary.contains(QStringLiteral("Java")));
    QVERIFY(candidates.first().entry.tags.contains(QStringLiteral("preference")));
    QCOMPARE(candidates.first().entry.source, QStringLiteral("user_explicit"));
}

void TestMemoryStrategy::testExtractorCreatesSemanticFromRememberStatement() {
    MemoryExtractor extractor;
    const QList<MemoryCandidate> candidates = extractor.extractFromUserInput(
        QStringLiteral("请记住 Desktop-Pet 使用 C++20 和 Qt6"),
        QStringLiteral("user_request"));

    QCOMPARE(candidates.size(), 1);
    QCOMPARE(candidates.first().operation, MemoryCandidateOperation::Write);
    QCOMPARE(candidates.first().entry.type, MemoryType::Semantic);
    QCOMPARE(candidates.first().entry.scope, QStringLiteral("user"));
    QVERIFY(candidates.first().entry.summary.contains(QStringLiteral("Desktop-Pet")));
    QVERIFY(candidates.first().entry.evidence.contains(QStringLiteral("请记住 Desktop-Pet 使用 C++20 和 Qt6")));
}

void TestMemoryStrategy::testExtractorCreatesForgetCandidate() {
    MemoryExtractor extractor;
    const QList<MemoryCandidate> candidates = extractor.extractFromUserInput(QStringLiteral("删除关于面试的记忆"), QStringLiteral("user_request"));

    QCOMPARE(candidates.size(), 1);
    QCOMPARE(candidates.first().operation, MemoryCandidateOperation::Forget);
    QCOMPARE(candidates.first().query, QStringLiteral("面试"));
    QVERIFY(candidates.first().explicitRequest);
}

void TestMemoryStrategy::testExtractorCreatesDaydreamImpressionForSelfDisclosure() {
    MemoryExtractor extractor;
    const MemoryEntry impression = extractor.extractDaydreamImpression(
        QStringLiteral("我最近在学习 Qt 6 的模型视图框架"), QStringLiteral("user_request"));

    QCOMPARE(impression.type, MemoryType::ShortTerm);
    QCOMPARE(impression.partition, QString()); // MemoryStore derives the physical partition.
    QCOMPARE(impression.privacyLevel, PrivacyLevel::Personal);
    QCOMPARE(impression.source, QStringLiteral("user_interaction"));
    QCOMPARE(impression.mentionCount, 1);
    QVERIFY(impression.key.startsWith(QStringLiteral("daydream:user:")));
    QVERIFY(impression.tags.contains(QStringLiteral("daydream_inbox")));
}

void TestMemoryStrategy::testExtractorRejectsUnsafeDaydreamImpressions() {
    MemoryExtractor extractor;
    QVERIFY(extractor.extractDaydreamImpression(
        QStringLiteral("帮我查一下天气"), QStringLiteral("user_request")).content.isEmpty());
    QVERIFY(extractor.extractDaydreamImpression(
        QStringLiteral("我的 API key 是 sk-abcdefghijklmnopqrstuvwxyz"),
        QStringLiteral("user_request")).content.isEmpty());
    QVERIFY(MemoryExtractor::isLikelySensitiveContent(
        QStringLiteral("我的银行卡是 6222 1234 5678 9012")));
}

void TestMemoryStrategy::testPolicyWritesAndSkipsDuplicate() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryExtractor extractor;
    MemoryPolicy policy;
    const QList<MemoryCandidate> candidates = extractor.extractFromUserInput(QStringLiteral("我喜欢 C++"), QStringLiteral("user_request"));

    const MemoryPolicyReport firstReport = policy.applyCandidates(candidates, &store);
    QCOMPARE(firstReport.written, 1);
    QCOMPARE(firstReport.skipped, 0);
    QCOMPARE(store.all().size(), 1);
    QCOMPARE(store.all().first().status, MemoryStatus::Active);

    const MemoryPolicyReport secondReport = policy.applyCandidates(candidates, &store);
    QCOMPARE(secondReport.written, 0);
    QCOMPARE(secondReport.skipped, 1);
    QCOMPARE(store.all().size(), 1);
}

void TestMemoryStrategy::testPolicyRejectsSensitiveMemory() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryExtractor extractor;
    MemoryPolicy policy;
    const QList<MemoryCandidate> candidates = extractor.extractFromUserInput(QStringLiteral("请记住我的 api key 是 abc123"), QStringLiteral("user_request"));

    QCOMPARE(candidates.size(), 1);
    QCOMPARE(candidates.first().entry.privacyLevel, PrivacyLevel::Sensitive);

    const MemoryPolicyReport report = policy.applyCandidates(candidates, &store);
    QCOMPARE(report.written, 0);
    QCOMPARE(report.skipped, 1);
    QCOMPARE(store.all().size(), 0);
}

void TestMemoryStrategy::testPolicyMarksMatchedMemoryDeleted() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryExtractor extractor;
    MemoryPolicy policy;

    const MemoryPolicyReport writeReport = policy.applyCandidates(
        extractor.extractFromUserInput(QStringLiteral("我喜欢 Java"), QStringLiteral("user_request")),
        &store);
    QCOMPARE(writeReport.written, 1);
    QCOMPARE(store.all().size(), 1);

    const MemoryPolicyReport forgetReport = policy.applyCandidates(
        extractor.extractFromUserInput(QStringLiteral("忘记 Java"), QStringLiteral("user_request")),
        &store);
    QCOMPARE(forgetReport.forgotten, 1);
    QCOMPARE(store.all().size(), 1);
    QCOMPARE(store.all().first().status, MemoryStatus::Deleted);
}

void TestMemoryStrategy::
stageCandidates_whenWriteSupersedesAndForget_shouldKeepGuiCacheAheadOfPersistence() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);
    MemoryEntry existing;
    existing.id = QStringLiteral("existing-preference");
    existing.type = MemoryType::Preference;
    existing.key = QStringLiteral("preference:language");
    existing.scope = QStringLiteral("preference");
    existing.summary = QStringLiteral("用户喜欢 Java");
    existing.content = existing.summary;
    existing.confidence = 0.95;
    existing.importance = 0.7;
    existing.strength = 0.7;
    const MemoryEntry stored = store.addEntry(existing);

    MemoryCandidate replacement;
    replacement.operation = MemoryCandidateOperation::Write;
    replacement.explicitRequest = true;
    replacement.entry.type = MemoryType::Preference;
    replacement.entry.key = existing.key;
    replacement.entry.scope = existing.scope;
    replacement.entry.summary = QStringLiteral("用户喜欢 C++");
    replacement.entry.content = replacement.entry.summary;
    replacement.entry.confidence = 0.96;
    replacement.entry.importance = 0.75;
    replacement.entry.strength = 0.75;

    MemoryPolicy policy;
    const StagedMemoryPolicyResult staged = policy.stageCandidates({replacement}, &store);
    QCOMPARE(staged.report.written, 1);
    QCOMPARE(store.findById(stored.id)->status, MemoryStatus::Superseded);
    QCOMPARE(store.all().size(), 2);
    const QString replacementId = store.all().last().id;
    QCOMPARE(store.findById(replacementId)->status, MemoryStatus::Active);

    MemoryStore beforeCommit;
    setupStoreWithDb(beforeCommit, tempDir);
    QCOMPARE(beforeCommit.all().size(), 1);
    QCOMPARE(beforeCommit.findById(stored.id)->status, MemoryStatus::Active);

    QVERIFY(store.persistMutationBatch(staged.mutations));
    MemoryStore afterCommit;
    setupStoreWithDb(afterCommit, tempDir);
    QCOMPARE(afterCommit.all().size(), 2);
    QCOMPARE(afterCommit.findById(stored.id)->status, MemoryStatus::Superseded);

    MemoryCandidate forget;
    forget.operation = MemoryCandidateOperation::Forget;
    forget.explicitRequest = true;
    forget.query = QStringLiteral("C++");
    forget.rawText = QStringLiteral("忘记 C++");
    const StagedMemoryPolicyResult forgotten = policy.stageCandidates({forget}, &store);
    QCOMPARE(forgotten.report.forgotten, 1);
    QCOMPARE(store.findById(replacementId)->status, MemoryStatus::Deleted);

    MemoryStore beforeForgetCommit;
    setupStoreWithDb(beforeForgetCommit, tempDir);
    QVERIFY(beforeForgetCommit.findById(replacementId));
    QCOMPARE(beforeForgetCommit.findById(replacementId)->status, MemoryStatus::Active);
    QVERIFY(store.persistMutationBatch(forgotten.mutations));
}

void TestMemoryStrategy::testStoreUpdateEntryByIdPersists() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry entry;
    entry.type = MemoryType::Semantic;
    entry.key = QStringLiteral("update:id");
    entry.summary = QStringLiteral("原始记忆");
    entry.content = entry.summary;
    entry.tags = {QStringLiteral("alpha")};
    entry.importance = 0.4;
    entry.confidence = 0.8;
    entry.strength = 0.4;
    const MemoryEntry stored = store.addEntry(entry);

    MemoryEntry updated = stored;
    updated.summary = QStringLiteral("更新后的记忆");
    updated.tags = {QStringLiteral("alpha"), QStringLiteral("beta")};
    updated.evidence = {QStringLiteral("evidence")};
    updated.strength = 0.9;
    QVERIFY(store.updateEntryById(updated));

    MemoryStore reloaded;
    setupStoreWithDb(reloaded, tempDir);
    const MemoryEntry* found = reloaded.findById(stored.id);
    QVERIFY(found);
    QCOMPARE(found->summary, QStringLiteral("更新后的记忆"));
    QCOMPARE(found->key, QStringLiteral("update:id"));
    QVERIFY(found->tags.contains(QStringLiteral("beta")));
    QVERIFY(found->evidence.contains(QStringLiteral("evidence")));
    QVERIFY(found->strength >= 0.9);
}

void TestMemoryStrategy::testStoreDoesNotMutateWhenPersistenceFails() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry original;
    original.type = MemoryType::Semantic;
    original.key = QStringLiteral("atomic:test");
    original.summary = QStringLiteral("original");
    original.content = original.summary;
    const MemoryEntry stored = store.addEntry(original);
    QVERIFY(!stored.id.isEmpty());

    QSqlDatabase db = QSqlDatabase::database(store.databaseConnectionName());
    QSqlQuery trigger(db);
    QVERIFY(trigger.exec(QStringLiteral(
        "CREATE TRIGGER reject_memory_write BEFORE INSERT ON memory_items "
        "BEGIN SELECT RAISE(ABORT, 'forced failure'); END")));

    MemoryEntry added = original;
    added.id.clear();
    added.key = QStringLiteral("atomic:new");
    QCOMPARE(store.addEntry(added).id, QString());
    QCOMPARE(store.all().size(), 1);

    MemoryEntry updated = stored;
    updated.summary = QStringLiteral("must not leak into memory");
    QVERIFY(!store.updateEntryById(updated));
    const MemoryEntry* current = store.findById(stored.id);
    QVERIFY(current);
    QCOMPARE(current->summary, QStringLiteral("original"));
}

void TestMemoryStrategy::testRepositoryClearRollsBackOnFailure() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    SQLiteMemoryRepository repository;
    QVERIFY(repository.open(tempDir.filePath(QStringLiteral("memory.db"))));

    MemoryEntry entry;
    entry.id = QStringLiteral("clear-rollback");
    entry.type = MemoryType::Semantic;
    entry.summary = QStringLiteral("must survive failed clear");
    entry.tags = {QStringLiteral("important")};
    entry.evidence = {QStringLiteral("source text")};
    QVERIFY(repository.insert(entry));

    QSqlDatabase db = QSqlDatabase::database(repository.connectionName());
    QSqlQuery trigger(db);
    QVERIFY(trigger.exec(QStringLiteral(
        "CREATE TRIGGER reject_memory_clear BEFORE DELETE ON memory_items "
        "BEGIN SELECT RAISE(ABORT, 'forced failure'); END")));

    QVERIFY(!repository.clear());
    const QList<MemoryEntry> entries = repository.loadAll();
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries.first().tags, QStringList{QStringLiteral("important")});
    QCOMPARE(entries.first().evidence, QStringList{QStringLiteral("source text")});
}

void TestMemoryStrategy::testSkillStoreDoesNotMutateWhenPersistenceFails() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString storagePath = tempDir.filePath(QStringLiteral("skills"));
    SkillStore store;
    store.setStoragePath(storagePath);
    QVERIFY(store.load());

    SkillEntry entry;
    entry.name = QStringLiteral("persistent skill");
    entry.description = QStringLiteral("original");
    const SkillEntry created = store.add(entry);
    QVERIFY(!created.id.isEmpty());

    const QString backupPath = tempDir.filePath(QStringLiteral("skills-backup"));
    QVERIFY(QDir().rename(storagePath, backupPath));
    QFile blocker(storagePath);
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.write("not a directory");
    blocker.close();

    SkillEntry updated = created;
    updated.description = QStringLiteral("must not leak into memory");
    QVERIFY(!store.update(updated));
    QCOMPARE(store.findById(created.id)->description, QStringLiteral("original"));

    QVERIFY(!store.recordOutcome(created.id, true));
    QCOMPARE(store.findById(created.id)->useCount, 0);

    SkillEntry second;
    second.name = QStringLiteral("failed add");
    QVERIFY(store.add(second).id.isEmpty());
    QCOMPARE(store.count(), 1);
}

void TestMemoryStrategy::testStoreUpdateStatusByIdIsExact() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry first;
    first.type = MemoryType::Preference;
    first.key = QStringLiteral("same:key");
    first.summary = QStringLiteral("第一条");
    first.importance = 0.5;
    first.confidence = 0.8;
    const MemoryEntry storedFirst = store.addEntry(first);

    MemoryEntry second = first;
    second.summary = QStringLiteral("第二条");
    const MemoryEntry storedSecond = store.addEntry(second);

    QJsonObject patch;
    patch[QStringLiteral("reason")] = QStringLiteral("test");
    QVERIFY(store.updateStatusById(storedSecond.id, MemoryStatus::Archived, patch));

    const MemoryEntry* firstAfter = store.findById(storedFirst.id);
    const MemoryEntry* secondAfter = store.findById(storedSecond.id);
    QVERIFY(firstAfter);
    QVERIFY(secondAfter);
    QCOMPARE(firstAfter->status, MemoryStatus::Active);
    QCOMPARE(secondAfter->status, MemoryStatus::Archived);
    QCOMPARE(secondAfter->payload.value(QStringLiteral("reason")).toString(), QStringLiteral("test"));
}

void TestMemoryStrategy::testRetrieverRanksKeywordAndPreferredType() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry javaPreference;
    javaPreference.type = MemoryType::Preference;
    javaPreference.key = QStringLiteral("preference:java");
    javaPreference.summary = QStringLiteral("用户喜欢 Java");
    javaPreference.content = javaPreference.summary;
    javaPreference.tags = {QStringLiteral("preference"), QStringLiteral("coding")};
    javaPreference.scope = QStringLiteral("preference");
    javaPreference.importance = 0.8;
    javaPreference.confidence = 0.95;
    javaPreference.strength = 0.8;
    store.addEntry(javaPreference);

    MemoryEntry cppFact;
    cppFact.type = MemoryType::Semantic;
    cppFact.key = QStringLiteral("project:cpp");
    cppFact.summary = QStringLiteral("Desktop-Pet 使用 C++20 和 Qt6");
    cppFact.content = cppFact.summary;
    cppFact.tags = {QStringLiteral("project")};
    cppFact.scope = QStringLiteral("project");
    cppFact.importance = 0.7;
    cppFact.confidence = 0.9;
    cppFact.strength = 0.7;
    store.addEntry(cppFact);

    MemoryRetriever retriever;
    MemoryQuery query;
    query.text = QStringLiteral("你记得我喜欢 Java 吗");
    query.preferredTypes = {MemoryType::Preference};
    query.limit = 2;

    const QList<RetrievedMemory> result = retriever.retrieve(store, query);
    QVERIFY(result.size() >= 1);
    QCOMPARE(result.first().entry.type, MemoryType::Preference);
    QVERIFY(result.first().entry.summary.contains(QStringLiteral("Java")));
}

void TestMemoryStrategy::testRetrieverFiltersSensitiveByDefault() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry sensitive;
    sensitive.type = MemoryType::Semantic;
    sensitive.key = QStringLiteral("secret:token");
    sensitive.summary = QStringLiteral("用户的 token 是 abc");
    sensitive.content = sensitive.summary;
    sensitive.privacyLevel = PrivacyLevel::Sensitive;
    sensitive.importance = 1.0;
    sensitive.confidence = 1.0;
    store.addEntry(sensitive);

    MemoryRetriever retriever;
    MemoryQuery query;
    query.text = QStringLiteral("token");
    query.limit = 5;

    QVERIFY(retriever.retrieve(store, query).isEmpty());

    query.includeSensitive = true;
    QCOMPARE(retriever.retrieve(store, query).size(), 1);
}

void TestMemoryStrategy::testRetrieverFormatsContextLines() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry preference;
    preference.type = MemoryType::Preference;
    preference.key = QStringLiteral("preference:style");
    preference.summary = QStringLiteral("用户希望技术方案先讲架构，再讲代码");
    preference.content = preference.summary;
    preference.scope = QStringLiteral("communication");
    preference.importance = 0.9;
    preference.confidence = 0.96;
    preference.strength = 0.9;
    store.addEntry(preference);

    MemoryRetriever retriever;
    MemoryQuery query;
    query.text = QStringLiteral("技术方案怎么讲");
    query.preferredTypes = {MemoryType::Preference};
    query.limit = 1;

    const QStringList lines = retriever.formatForContext(retriever.retrieve(store, query));
    QCOMPARE(lines.size(), 1);
    QVERIFY(lines.first().startsWith(QStringLiteral("1. [preference/高置信度/communication]")));
    QVERIFY(lines.first().contains(QStringLiteral("先讲架构")));
}

// ---- Phase 3: Relation Graph tests ----

void TestMemoryStrategy::testPolicyCreatesSupersedes() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryPolicy policy;
    MemoryExtractor extractor;

    policy.applyCandidates(
        extractor.extractFromUserInput(QStringLiteral("我喜欢 Java"), QStringLiteral("user_request")),
        &store);
    QCOMPARE(store.all().size(), 1);
    const QString oldId = store.all().first().id;

    // Change key to force different key — supersedes won't trigger with different key
    // Instead, write same scope+type with explicit same key
    MemoryCandidate candidate;
    candidate.operation = MemoryCandidateOperation::Write;
    candidate.explicitRequest = true;
    candidate.entry.type = MemoryType::Preference;
    candidate.entry.key = store.all().first().key;
    candidate.entry.scope = QStringLiteral("preference");
    candidate.entry.summary = QStringLiteral("用户喜欢 Python");
    candidate.entry.content = candidate.entry.summary;
    candidate.entry.source = QStringLiteral("user_explicit");
    candidate.entry.confidence = 0.98;
    candidate.entry.importance = 0.72;
    candidate.entry.strength = 0.72;
    candidate.entry.tags = {QStringLiteral("preference")};

    const MemoryPolicyReport report = policy.applyCandidates({candidate}, &store);
    QCOMPARE(report.written, 1);
    QVERIFY(report.relationsCreated >= 1);

    // Old memory should be superseded, while the new same-key memory stays active.
    const MemoryEntry* old = store.findById(oldId);
    QVERIFY(old);
    QCOMPARE(old->status, MemoryStatus::Superseded);
    const MemoryEntry* newest = store.findById(store.all().last().id);
    QVERIFY(newest);
    QCOMPARE(newest->status, MemoryStatus::Active);

    // Relation should exist
    QVERIFY(store.relationGraph().hasRelation(store.all().last().id, oldId, MemoryRelationType::Supersedes));
}

void TestMemoryStrategy::testPolicyCreatesConflictsWith() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryPolicy policy;
    MemoryExtractor extractor;

    policy.applyCandidates(
        extractor.extractFromUserInput(QStringLiteral("我喜欢 Java"), QStringLiteral("user_request")),
        &store);

    policy.applyCandidates(
        extractor.extractFromUserInput(QStringLiteral("我不喜欢 Java"), QStringLiteral("user_request")),
        &store);

    const QList<MemoryRelation> relations = store.relationGraph().all();
    bool foundConflict = false;
    for (const MemoryRelation& rel : relations) {
        if (rel.type == MemoryRelationType::ConflictsWith) {
            foundConflict = true;
            break;
        }
    }
    QVERIFY(foundConflict);
}

void TestMemoryStrategy::testPolicyCreatesRelatedByTags() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry a;
    a.type = MemoryType::Semantic;
    a.key = QStringLiteral("fact:a");
    a.summary = QStringLiteral("事实 A");
    a.tags = {QStringLiteral("coding"), QStringLiteral("java"), QStringLiteral("preference")};
    a.importance = 0.5;
    a.confidence = 0.9;
    a.strength = 0.5;
    store.addEntry(a);

    MemoryCandidate candidate;
    candidate.operation = MemoryCandidateOperation::Write;
    candidate.explicitRequest = true;
    candidate.entry.type = MemoryType::Semantic;
    candidate.entry.key = QStringLiteral("fact:b");
    candidate.entry.summary = QStringLiteral("事实 B");
    candidate.entry.content = candidate.entry.summary;
    candidate.entry.tags = {QStringLiteral("coding"), QStringLiteral("java")};
    candidate.entry.importance = 0.5;
    candidate.entry.confidence = 0.9;
    candidate.entry.strength = 0.5;

    MemoryPolicy policy;
    const MemoryPolicyReport report = policy.applyCandidates({candidate}, &store);
    QCOMPARE(report.written, 1);
    QVERIFY(report.relationsCreated >= 1);

    bool foundRelated = false;
    for (const MemoryRelation& rel : store.relationGraph().all()) {
        if (rel.type == MemoryRelationType::Related) {
            foundRelated = true;
            break;
        }
    }
    QVERIFY(foundRelated);
}

void TestMemoryStrategy::testPolicyCreatesMentionedWith() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryCandidate a;
    a.operation = MemoryCandidateOperation::Write;
    a.explicitRequest = true;
    a.entry.type = MemoryType::Semantic;
    a.entry.key = QStringLiteral("mentioned:a");
    a.entry.summary = QStringLiteral("A");
    a.entry.content = QStringLiteral("A");
    a.entry.confidence = 0.98;
    a.entry.importance = 0.5;
    a.entry.strength = 0.5;

    MemoryCandidate b = a;
    b.entry.key = QStringLiteral("mentioned:b");
    b.entry.summary = QStringLiteral("B");
    b.entry.content = QStringLiteral("B");

    MemoryPolicy policy;
    const MemoryPolicyReport report = policy.applyCandidates({a, b}, &store);
    QCOMPARE(report.written, 2);

    bool foundMentioned = false;
    for (const MemoryRelation& rel : store.relationGraph().all()) {
        if (rel.type == MemoryRelationType::MentionedWith) {
            foundMentioned = true;
            break;
        }
    }
    QVERIFY(foundMentioned);
}

void TestMemoryStrategy::testPolicyFirstOfScopeImportanceBoost() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryCandidate candidate;
    candidate.operation = MemoryCandidateOperation::Write;
    candidate.explicitRequest = true;
    candidate.entry.type = MemoryType::Preference;
    candidate.entry.key = QStringLiteral("first:test");
    candidate.entry.scope = QStringLiteral("unique_scope");
    candidate.entry.summary = QStringLiteral("first memory");
    candidate.entry.content = candidate.entry.summary;
    candidate.entry.importance = 0.5;
    candidate.entry.confidence = 0.9;
    candidate.entry.strength = 0.5;

    MemoryPolicy policy;
    policy.applyCandidates({candidate}, &store);
    QCOMPARE(store.all().size(), 1);
    QVERIFY(store.all().first().importance >= 0.65);
}

// ---- Phase 4: Decay / Emotion / Reinforcement tests ----

void TestMemoryStrategy::testRetrieverDecayCurve() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry recent;
    recent.type = MemoryType::Episodic;
    recent.key = QStringLiteral("recent:event");
    recent.summary = QStringLiteral("最近的事件 alpha");
    recent.content = recent.summary;
    recent.importance = 0.5;
    recent.confidence = 0.8;
    recent.strength = 0.8;
    recent.updatedAt = QDateTime::currentDateTimeUtc();
    store.addEntry(recent);

    MemoryEntry old;
    old.type = MemoryType::Episodic;
    old.key = QStringLiteral("old:event");
    old.summary = QStringLiteral("很久前的事件 alpha");
    old.content = old.summary;
    old.importance = 0.5;
    old.confidence = 0.8;
    old.strength = 0.8;
    old.updatedAt = QDateTime::currentDateTimeUtc().addDays(-60);
    old.lastAccessedAt = old.updatedAt;
    store.addEntry(old);

    MemoryRetriever retriever;
    MemoryQuery query;
    query.text = QStringLiteral("alpha");
    query.limit = 2;

    const QList<RetrievedMemory> result = retriever.retrieve(store, query);
    QCOMPARE(result.size(), 2);
    QCOMPARE(result.first().entry.key, QStringLiteral("recent:event"));
}

void TestMemoryStrategy::testRetrieverEmotionBoost() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry happy;
    happy.type = MemoryType::Episodic;
    happy.key = QStringLiteral("happy:event");
    happy.summary = QStringLiteral("开心的回忆 beta");
    happy.content = happy.summary;
    happy.importance = 0.5;
    happy.confidence = 0.8;
    happy.strength = 0.5;
    happy.emotion = EmotionType::Joy;
    happy.emotionIntensity = 0.9;
    store.addEntry(happy);

    MemoryEntry sad;
    sad.type = MemoryType::Episodic;
    sad.key = QStringLiteral("sad:event");
    sad.summary = QStringLiteral("悲伤的回忆 beta");
    sad.content = sad.summary;
    sad.importance = 0.5;
    sad.confidence = 0.8;
    sad.strength = 0.5;
    sad.emotion = EmotionType::Sadness;
    sad.emotionIntensity = 0.9;
    store.addEntry(sad);

    MemoryRetriever retriever;
    MemoryQuery query;
    query.text = QStringLiteral("beta");
    query.limit = 2;
    query.currentEmotion = EmotionType::Joy;
    query.currentEmotionIntensity = 0.8;

    const QList<RetrievedMemory> result = retriever.retrieve(store, query);
    QCOMPARE(result.size(), 2);
    QCOMPARE(result.first().entry.key, QStringLiteral("happy:event"));
    const double scoreDifference = result.first().score - result.last().score;
    QVERIFY(scoreDifference > 0.0);
    QVERIFY(scoreDifference <= 0.35 + 1e-9);
}

void TestMemoryStrategy::testRetrieverReinforcement() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry entry;
    entry.type = MemoryType::Semantic;
    entry.key = QStringLiteral("reinforce:test");
    entry.summary = QStringLiteral("测试巩固 gamma");
    entry.content = entry.summary;
    entry.importance = 0.5;
    entry.confidence = 0.8;
    entry.strength = 0.5;
    entry.accessCount = 0;
    const MemoryEntry stored = store.addEntry(entry);

    MemoryRetriever retriever;
    MemoryQuery query;
    query.text = QStringLiteral("gamma");
    query.limit = 1;

    retriever.retrieve(store, query);

    const MemoryEntry* reinforced = store.findById(stored.id);
    QVERIFY(reinforced);
    QVERIFY(reinforced->strength >= 0.6);
    QCOMPARE(reinforced->accessCount, 1);
    QVERIFY(reinforced->lastAccessedAt.isValid());
}

void TestMemoryStrategy::testRetrieverReinforcementPersists() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry entry;
    entry.type = MemoryType::Semantic;
    entry.key = QStringLiteral("reinforce:persist");
    entry.summary = QStringLiteral("持久化巩固 theta");
    entry.content = entry.summary;
    entry.importance = 0.5;
    entry.confidence = 0.8;
    entry.strength = 0.5;
    const MemoryEntry stored = store.addEntry(entry);

    MemoryRetriever retriever;
    MemoryQuery query;
    query.text = QStringLiteral("theta");
    query.limit = 1;
    retriever.retrieve(store, query);

    MemoryStore reloaded;
    setupStoreWithDb(reloaded, tempDir);
    const MemoryEntry* reinforced = reloaded.findById(stored.id);
    QVERIFY(reinforced);
    QCOMPARE(reinforced->accessCount, 1);
    QVERIFY(reinforced->strength >= 0.6);
    QVERIFY(reinforced->lastAccessedAt.isValid());
}

void TestMemoryStrategy::
stageReinforcement_whenIdsExist_shouldUpdateMemoryAndReturnOneBatch() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MemoryStore store;
    setupStoreWithDb(store, directory);

    MemoryEntry first;
    first.type = MemoryType::Preference;
    first.key = QStringLiteral("reinforcement:first");
    first.summary = QStringLiteral("first staged reinforcement");
    first.content = first.summary;
    first.strength = 0.4;
    first.confidence = 0.9;
    const MemoryEntry storedFirst = store.addEntry(first);

    MemoryEntry second = first;
    second.key = QStringLiteral("reinforcement:second");
    second.summary = QStringLiteral("second staged reinforcement");
    second.content = second.summary;
    const MemoryEntry storedSecond = store.addEntry(second);
    const QDateTime accessedAt = QDateTime::currentDateTimeUtc();

    const MemoryReinforcementBatch batch = store.stageReinforcement(
        {storedFirst.id, storedSecond.id}, accessedAt);

    QCOMPARE(batch.entries.size(), 2);
    QCOMPARE(store.findById(storedFirst.id)->accessCount, 1);
    QCOMPARE(store.findById(storedSecond.id)->accessCount, 1);
    QCOMPARE(store.findById(storedFirst.id)->lastAccessedAt, accessedAt);

    MemoryStore persisted;
    setupStoreWithDb(persisted, directory);
    QCOMPARE(persisted.findById(storedFirst.id)->accessCount, 0);
    QCOMPARE(persisted.findById(storedSecond.id)->accessCount, 0);
}

void TestMemoryStrategy::
stageReinforcement_whenIdsRepeatOrAreMissing_shouldUpdateEachKnownEntryOnce() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    MemoryStore store;
    setupStoreWithDb(store, directory);

    MemoryEntry active;
    active.type = MemoryType::Semantic;
    active.key = QStringLiteral("reinforcement:active");
    active.summary = QStringLiteral("active staged reinforcement");
    active.content = active.summary;
    active.strength = 0.4;
    const MemoryEntry storedActive = store.addEntry(active);

    MemoryEntry deleted = active;
    deleted.key = QStringLiteral("reinforcement:deleted");
    deleted.summary = QStringLiteral("deleted staged reinforcement");
    deleted.content = deleted.summary;
    deleted.status = MemoryStatus::Deleted;
    const MemoryEntry storedDeleted = store.addEntry(deleted);

    const MemoryReinforcementBatch batch = store.stageReinforcement(
        {storedActive.id, QStringLiteral("missing"), storedActive.id,
         storedDeleted.id});

    QCOMPARE(batch.entries.size(), 1);
    QCOMPARE(store.findById(storedActive.id)->accessCount, 1);
    QCOMPARE(store.findById(storedDeleted.id)->accessCount, 0);
    QCOMPARE(store.findById(storedDeleted.id)->status, MemoryStatus::Deleted);
}

void TestMemoryStrategy::testRetrieverGraphExpansion() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry main;
    main.type = MemoryType::Preference;
    main.key = QStringLiteral("pref:main");
    main.summary = QStringLiteral("用户喜欢 delta 语言");
    main.content = main.summary;
    main.importance = 0.8;
    main.confidence = 0.95;
    main.strength = 0.8;
    const MemoryEntry storedMain = store.addEntry(main);

    MemoryEntry related;
    related.type = MemoryType::Semantic;
    related.key = QStringLiteral("fact:related");
    related.summary = QStringLiteral("该语言的框架推荐");
    related.content = related.summary;
    related.importance = 0.3;
    related.confidence = 0.7;
    related.strength = 0.3;
    const MemoryEntry storedRelated = store.addEntry(related);

    MemoryRelation rel;
    rel.fromMemoryId = storedMain.id;
    rel.toMemoryId = storedRelated.id;
    rel.type = MemoryRelationType::Related;
    rel.weight = 0.8;
    store.relationGraph().addRelation(rel);

    MemoryRetriever retriever;
    MemoryQuery query;
    query.text = QStringLiteral("delta");
    query.preferredTypes = {MemoryType::Preference};
    query.limit = 5;

    const QList<RetrievedMemory> result = retriever.retrieve(store, query);
    QVERIFY(result.size() >= 2);

    bool foundExpanded = false;
    for (const RetrievedMemory& mem : result) {
        if (mem.entry.key == QStringLiteral("fact:related") && mem.fromGraphExpansion) {
            foundExpanded = true;
            break;
        }
    }
    QVERIFY(foundExpanded);
}

void TestMemoryStrategy::testMemoryOrganizeDryRun() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry expired;
    expired.type = MemoryType::ShortTerm;
    expired.key = QStringLiteral("assistant_response");
    expired.summary = QStringLiteral("需要过期的短期记忆");
    expired.content = expired.summary;
    expired.expiresAt = QDateTime::currentDateTimeUtc().addSecs(-60);
    expired.importance = 0.4;
    expired.confidence = 0.8;
    expired.strength = 0.4;
    const MemoryEntry stored = store.addEntry(expired);

    MemoryOrganizeTool tool(&store);
    QJsonObject params;
    params[QStringLiteral("dry_run")] = 1;
    const ToolResult result = tool.execute(params);
    QVERIFY(result.success);
    QVERIFY(result.data.value(QStringLiteral("expired")).toInt() >= 1);
    QVERIFY(result.data.value(QStringLiteral("changed")).toInt() >= 1);

    const MemoryEntry* after = store.findById(stored.id);
    QVERIFY(after);
    QCOMPARE(after->status, MemoryStatus::Active);
}

void TestMemoryStrategy::testMemoryOrganizeExpiresWithoutDeleting() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry entry;
    entry.type = MemoryType::ShortTerm;
    entry.key = QStringLiteral("expire:test");
    entry.summary = QStringLiteral("过期但不删除");
    entry.content = entry.summary;
    entry.expiresAt = QDateTime::currentDateTimeUtc().addSecs(-60);
    entry.importance = 0.4;
    entry.confidence = 0.8;
    entry.strength = 0.4;
    const MemoryEntry stored = store.addEntry(entry);

    MemoryOrganizeTool tool(&store);
    QJsonObject params;
    params[QStringLiteral("mode")] = QStringLiteral("expire");
    const ToolResult result = tool.execute(params);
    QVERIFY(result.success);
    QCOMPARE(result.data.value(QStringLiteral("expired")).toInt(), 1);

    const MemoryEntry* after = store.findById(stored.id);
    QVERIFY(after);
    QCOMPARE(after->status, MemoryStatus::Expired);
    QVERIFY(after->status != MemoryStatus::Deleted);
}

void TestMemoryStrategy::testMemoryOrganizeArchivesOperationalEvents() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry event;
    event.type = MemoryType::Event;
    event.key = QStringLiteral("tool_execution");
    event.summary = QStringLiteral("旧工具执行记录");
    event.content = event.summary;
    event.createdAt = QDateTime::currentDateTimeUtc().addDays(-10);
    event.updatedAt = event.createdAt;
    event.importance = 0.2;
    event.confidence = 0.8;
    event.strength = 0.2;
    const MemoryEntry stored = store.addEntry(event);

    MemoryOrganizeTool tool(&store);
    QJsonObject params;
    params[QStringLiteral("mode")] = QStringLiteral("archive");
    const ToolResult result = tool.execute(params);
    QVERIFY(result.success);
    QCOMPARE(result.data.value(QStringLiteral("archived")).toInt(), 1);

    const MemoryEntry* after = store.findById(stored.id);
    QVERIFY(after);
    QCOMPARE(after->status, MemoryStatus::Archived);
}

void TestMemoryStrategy::testMemoryOrganizeMergesDuplicates() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry first;
    first.type = MemoryType::Semantic;
    first.key = QStringLiteral("dup:first");
    first.scope = QStringLiteral("project");
    first.summary = QStringLiteral("重复记忆内容");
    first.content = first.summary;
    first.tags = {QStringLiteral("memory"), QStringLiteral("test")};
    first.importance = 0.8;
    first.confidence = 0.9;
    first.strength = 0.8;
    const MemoryEntry storedFirst = store.addEntry(first);

    MemoryEntry second = first;
    second.key = QStringLiteral("dup:second");
    second.importance = 0.3;
    second.tags = {QStringLiteral("test"), QStringLiteral("duplicate")};
    const MemoryEntry storedSecond = store.addEntry(second);

    MemoryOrganizeTool tool(&store);
    QJsonObject params;
    params[QStringLiteral("mode")] = QStringLiteral("merge_duplicates");
    const ToolResult result = tool.execute(params);
    QVERIFY(result.success);
    QCOMPARE(result.data.value(QStringLiteral("superseded")).toInt(), 1);
    QVERIFY(!QString::fromUtf8(QJsonDocument(result.data).toJson(QJsonDocument::Compact)).contains(QStringLiteral("重复记忆内容")));

    const MemoryEntry* firstAfter = store.findById(storedFirst.id);
    const MemoryEntry* secondAfter = store.findById(storedSecond.id);
    QVERIFY(firstAfter);
    QVERIFY(secondAfter);

    const MemoryEntry* active = firstAfter->status == MemoryStatus::Active ? firstAfter : secondAfter;
    const MemoryEntry* duplicate = firstAfter->status == MemoryStatus::Superseded ? firstAfter : secondAfter;
    QCOMPARE(active->status, MemoryStatus::Active);
    QCOMPARE(duplicate->status, MemoryStatus::Superseded);
    QVERIFY(store.relationGraph().hasRelation(active->id, duplicate->id, MemoryRelationType::Supersedes));
}

void TestMemoryStrategy::testMemoryOrganizeSkipsSensitiveOutput() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry sensitive;
    sensitive.type = MemoryType::Semantic;
    sensitive.key = QStringLiteral("secret:test");
    sensitive.summary = QStringLiteral("超级秘密 token abc");
    sensitive.content = sensitive.summary;
    sensitive.privacyLevel = PrivacyLevel::Sensitive;
    sensitive.importance = 0.9;
    sensitive.confidence = 0.9;
    sensitive.strength = 0.9;
    store.addEntry(sensitive);

    MemoryOrganizeTool tool(&store);
    QJsonObject params;
    params[QStringLiteral("dry_run")] = 1;
    const ToolResult result = tool.execute(params);
    QVERIFY(result.success);
    QCOMPARE(result.data.value(QStringLiteral("skipped_sensitive")).toInt(), 1);

    const QString payload = QString::fromUtf8(QJsonDocument(result.data).toJson(QJsonDocument::Compact));
    QVERIFY(!payload.contains(QStringLiteral("超级秘密")));
    QVERIFY(!payload.contains(QStringLiteral("abc")));
}

// ---- Phase 5: Working Memory Cache tests ----

void TestMemoryStrategy::testWorkingMemoryCacheTtl() {
    WorkingMemoryCache cache;

    WorkingMemoryItem item;
    item.summary = QStringLiteral("短期记忆");
    item.content = item.summary;
    item.source = QStringLiteral("tool_result");
    item.createdAt = QDateTime::currentDateTimeUtc().addSecs(-1000);
    item.expiresAt = QDateTime::currentDateTimeUtc().addSecs(-1);
    cache.add(item);

    QCOMPARE(cache.size(), 1);
    cache.cleanup();
    QCOMPARE(cache.size(), 0);
}

void TestMemoryStrategy::testWorkingMemoryCacheCapacity() {
    WorkingMemoryCache cache;
    cache.setCapacity(3);

    for (int i = 0; i < 5; ++i) {
        WorkingMemoryItem item;
        item.summary = QStringLiteral("item_%1").arg(i);
        item.content = item.summary;
        item.importance = i * 0.1;
        cache.add(item);
    }

    QCOMPARE(cache.size(), 3);
    bool hasHighest = false;
    for (const WorkingMemoryItem& item : cache.all()) {
        if (item.summary == QStringLiteral("item_4")) hasHighest = true;
    }
    QVERIFY(hasHighest);
}

void TestMemoryStrategy::testWorkingMemoryCacheConsolidation() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    WorkingMemoryCache cache;

    WorkingMemoryItem item;
    item.summary = QStringLiteral("反复提及的话题");
    item.content = item.summary;
    item.source = QStringLiteral("topic");
    item.importance = 0.5;
    item.createdAt = QDateTime::currentDateTimeUtc().addSecs(-100);
    item.expiresAt = QDateTime::currentDateTimeUtc().addSecs(-1);

    cache.add(item);
    cache.add(item); // mentionCount becomes 2

    QCOMPARE(cache.size(), 1);
    QCOMPARE(cache.all().first().mentionCount, 2);

    cache.cleanup(&store);
    QCOMPARE(cache.size(), 0);
    QCOMPARE(store.all().size(), 1);
    QCOMPARE(store.all().first().source, QStringLiteral("consolidation"));
}

void TestMemoryStrategy::testWorkingMemoryRetrieverIntegration() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    WorkingMemoryCache cache;
    WorkingMemoryItem wm;
    wm.summary = QStringLiteral("刚才讨论了 epsilon 架构");
    wm.content = wm.summary;
    wm.tags = {QStringLiteral("topic")};
    wm.source = QStringLiteral("topic");
    wm.importance = 0.5;
    cache.add(wm);

    MemoryRetriever retriever;
    MemoryQuery query;
    query.text = QStringLiteral("epsilon");
    query.limit = 5;

    const QList<RetrievedMemory> result = retriever.retrieve(store, query, &cache);
    QCOMPARE(result.size(), 1);
    QVERIFY(result.first().entry.id.startsWith(QStringLiteral("wm:")));
    QVERIFY(result.first().reasons.contains(QStringLiteral("working_memory")));
}

// ---- Phase 6: Embedding no-op test ----

void TestMemoryStrategy::testNoopEmbeddingIndexDoesNotAffectRetrieval() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry entry;
    entry.type = MemoryType::Semantic;
    entry.key = QStringLiteral("embed:test");
    entry.summary = QStringLiteral("嵌入测试 zeta");
    entry.content = entry.summary;
    entry.importance = 0.5;
    entry.confidence = 0.8;
    entry.strength = 0.5;
    store.addEntry(entry);

    NoopEmbeddingIndex noopIndex;
    MemoryRetriever retriever;
    MemoryQuery query;
    query.text = QStringLiteral("zeta");
    query.limit = 5;

    const QList<RetrievedMemory> withoutEmbed = retriever.retrieve(store, query);
    const QList<RetrievedMemory> withEmbed = retriever.retrieve(store, query, nullptr, &noopIndex);

    QCOMPARE(withoutEmbed.size(), withEmbed.size());
    QVERIFY(!withoutEmbed.isEmpty());
}

// 9 种 MemoryType → 分区映射（memory_improvement_plan.md B3）。
// Core 类型并入 Semantic（靠自适应近不朽，非独立冻结分区）；
// Preference/Semantic 直映；Episodic+Event→Episodic；
// Working/ShortTerm/TaskShadow→Hippocampus；Relationship→Semantic。
void TestMemoryStrategy::testPartitionMappingForAllTypes() {
    QCOMPARE(partitionForType(MemoryType::Core), MemoryPartition::Semantic);
    QCOMPARE(partitionForType(MemoryType::Preference), MemoryPartition::Preference);
    QCOMPARE(partitionForType(MemoryType::Procedural), MemoryPartition::Procedural);
    QCOMPARE(partitionForType(MemoryType::Semantic), MemoryPartition::Semantic);
    QCOMPARE(partitionForType(MemoryType::Episodic), MemoryPartition::Episodic);
    QCOMPARE(partitionForType(MemoryType::Event), MemoryPartition::Episodic);
    QCOMPARE(partitionForType(MemoryType::Working), MemoryPartition::Hippocampus);
    QCOMPARE(partitionForType(MemoryType::ShortTerm), MemoryPartition::Hippocampus);
    QCOMPARE(partitionForType(MemoryType::TaskShadow), MemoryPartition::Hippocampus);
    QCOMPARE(partitionForType(MemoryType::Relationship), MemoryPartition::Semantic);

    // 分区字符串往返（5 个分区）
    for (MemoryPartition p : {MemoryPartition::Hippocampus, MemoryPartition::Episodic,
                              MemoryPartition::Semantic, MemoryPartition::Preference,
                              MemoryPartition::Procedural}) {
        QCOMPARE(partitionFromString(partitionToString(p)), p);
    }
    // 旧库残留 "core" 值兼容 → 并入 Semantic
    QCOMPARE(partitionFromString(QStringLiteral("core")), MemoryPartition::Semantic);

    // 仅 Hippocampus 不清扫；其余四个清扫
    QVERIFY(!policyFor(MemoryPartition::Hippocampus).sweepEnabled);
    QVERIFY(policyFor(MemoryPartition::Episodic).sweepEnabled);
    QVERIFY(policyFor(MemoryPartition::Semantic).sweepEnabled);
    QVERIFY(policyFor(MemoryPartition::Procedural).sweepEnabled);
    QVERIFY(policyFor(MemoryPartition::Preference).sweepEnabled);
}

// 自适应衰减：对齐 hebb-mind 真实参数验算 + Hippocampus 不清扫 + access 拉伸半衰期
// + Core 类型靠高 importance 近不朽（无独立冻结分区）。
void TestMemoryStrategy::testAdaptiveDecayRetentionAndForgetDays() {
    // hebb 算例：Semantic importance=8 access=10 → eff=441 天 → idle≈441·ln(1/0.3)≈531 天
    const auto semantic = policyFor(MemoryPartition::Semantic);
    const double eff = semantic.effectiveHalfLife(8.0, 10);
    QCOMPARE(eff, 441.0);
    const double forget = semantic.forgetIdleDays(8.0, 10);
    QVERIFY(qFuzzyCompare(forget, 441.0 * std::log(1.0 / 0.3)));

    // Episodic importance=3 access=1 → eff=42 天
    const auto episodic = policyFor(MemoryPartition::Episodic);
    QCOMPARE(episodic.effectiveHalfLife(3.0, 1), 42.0);

    // 留存率随 idle 单调下降，idle=0 时=1
    QCOMPARE(episodic.retention(3.0, 1, 0.0), 1.0);
    QVERIFY(episodic.retention(3.0, 1, 30.0) > episodic.retention(3.0, 1, 60.0));

    // access 更高 → 半衰期更长 → 同 idle 留存更高
    QVERIFY(episodic.retention(3.0, 10, 30.0) > episodic.retention(3.0, 1, 30.0));

    // Hippocampus 不清扫：retention 恒 1，forgetIdleDays < 0
    const auto hippo = policyFor(MemoryPartition::Hippocampus);
    QCOMPARE(hippo.retention(5.0, 0, 365.0), 1.0);
    QVERIFY(hippo.forgetIdleDays(5.0, 0) < 0.0);

    // Core 类型保护：落入 Semantic，高 importance(importance=10/对应entry 1.0)+access
    // 即使 idle 365 天，retention 仍远高于 threshold，靠自适应近不朽（无需冻结分区）。
    // eff = 90×(1+3.0×1.0+1.5×1.0) = 90×5.5 = 495 天；retention(365)=exp(-365/495)≈0.48 > 0.3
    const double coreEff = semantic.effectiveHalfLife(10.0, 10);
    QCOMPARE(coreEff, 495.0);
    QVERIFY(semantic.retention(10.0, 10, 365.0) > 0.3);

    // importance=0 仅不增益，不是删除信号：仍按 base 半衰期遗忘
    const double effZero = episodic.effectiveHalfLife(0.0, 0);
    QCOMPARE(effZero, 30.0);  // base 不变
}

// 分区持久化：addEntry 派生 partition → 落 SQLite → 重 load 后仍正确；JSON 往返亦保持。
void TestMemoryStrategy::testPartitionPersistedAndBackfilled() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    MemoryStore store;
    setupStoreWithDb(store, dir);

    MemoryEntry semantic;
    semantic.type = MemoryType::Semantic;
    semantic.key = QStringLiteral("lang:cpp");
    semantic.summary = QStringLiteral("用户用 C++20");
    const MemoryEntry storedSemantic = store.addEntry(semantic);

    MemoryEntry working;
    working.type = MemoryType::ShortTerm;
    working.summary = QStringLiteral("临时提到番茄");
    const MemoryEntry storedWorking = store.addEntry(working);

    MemoryEntry procedural;
    procedural.type = MemoryType::Procedural;
    procedural.summary = QStringLiteral("用 CMake 构建桌宠");
    const MemoryEntry storedProcedural = store.addEntry(procedural);

    // addEntry 返回派生 partition 后的副本
    QCOMPARE(storedSemantic.partition, QStringLiteral("semantic"));
    QCOMPARE(storedWorking.partition, QStringLiteral("hippocampus"));
    QCOMPARE(storedProcedural.partition, QStringLiteral("procedural"));

    // JSON 往返保持 partition
    const QJsonObject obj = storedSemantic.toJson();
    QCOMPARE(obj.value("partition").toString(), QStringLiteral("semantic"));
    const MemoryEntry fromJson = MemoryEntry::fromJson(obj);
    QCOMPARE(fromJson.partition, QStringLiteral("semantic"));

    // 旧式无 partition 字段的 JSON，fromJson 应派生自 type
    QJsonObject legacy = storedSemantic.toJson();
    legacy.remove("partition");
    QCOMPARE(MemoryEntry::fromJson(legacy).partition, QStringLiteral("semantic"));

    // 重新 load（走 SQLite loadAll），partition 应持久化
    MemoryStore reloaded;
    setupStoreWithDb(reloaded, dir);
    const auto all = reloaded.all();
    QVERIFY(all.size() >= 3);
    bool foundSemantic = false, foundWorking = false, foundProcedural = false;
    for (const MemoryEntry& e : all) {
        if (e.type == MemoryType::Semantic) {
            QCOMPARE(e.partition, QStringLiteral("semantic"));
            foundSemantic = true;
        }
        if (e.type == MemoryType::ShortTerm) {
            QCOMPARE(e.partition, QStringLiteral("hippocampus"));
            foundWorking = true;
        }
        if (e.type == MemoryType::Procedural) {
            QCOMPARE(e.partition, QStringLiteral("procedural"));
            foundProcedural = true;
        }
    }
    QVERIFY(foundSemantic);
    QVERIFY(foundWorking);
    QVERIFY(foundProcedural);
}

void TestMemoryStrategy::testLegacySchemaWithoutPartitionMigratesBeforeIndexCreation() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString databasePath = dir.filePath(QStringLiteral("memory.db"));

    {
        SQLiteMemoryRepository repository;
        QVERIFY(repository.open(databasePath));
        MemoryEntry entry;
        entry.id = QStringLiteral("legacy-semantic");
        entry.type = MemoryType::Semantic;
        entry.summary = QStringLiteral("legacy row");
        QVERIFY(repository.insert(entry));
    }

    const QString connectionName = QStringLiteral("legacy_partition_migration");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(
            QStringLiteral("QSQLITE"), connectionName);
        database.setDatabaseName(databasePath);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("DROP INDEX idx_memory_items_partition")));
        QVERIFY(query.exec(QStringLiteral(
            "ALTER TABLE memory_items DROP COLUMN partition")));
        QVERIFY(query.exec(QStringLiteral("PRAGMA user_version=0")));
        database.close();
    }
    QSqlDatabase::removeDatabase(connectionName);

    MemoryStore migrated;
    migrated.setDatabasePath(databasePath);
    migrated.setStoragePath(dir.filePath(QStringLiteral("memory.json")));
    QString error;
    QVERIFY2(migrated.load(&error), qPrintable(error));
    QCOMPARE(migrated.all().size(), 1);
    QCOMPARE(migrated.all().first().partition, QStringLiteral("semantic"));
}

// 自适应遗忘扫描：高空闲低重要 Episodic 被 Expired；Core 类型(高 importance)靠自适应保留；
// Sensitive 跳过；不物理删除。
void TestMemoryStrategy::testForgettingSweepExpiresStaleAndSparesImportant() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    // Episodic，最后访问 90 天前，importance 0.5、access 0
    // → eff=30×(1+1.0×0.5)=45，retention=exp(-90/45)≈0.135 < 0.3 → 应遗忘
    MemoryEntry stale;
    stale.type = MemoryType::Episodic;
    stale.key = QStringLiteral("stale:event");
    stale.summary = QStringLiteral("很久没想起 gamma");
    stale.content = stale.summary;
    stale.importance = 0.5;
    stale.confidence = 0.8;
    stale.strength = 0.8;
    stale.updatedAt = QDateTime::currentDateTimeUtc().addDays(-90);
    stale.lastAccessedAt = stale.updatedAt;
    store.addEntry(stale);

    // Core 类型（用户身份），同样 90 天前 —— 落入 Semantic，addEntry 设 importance 下限 0.8
    // → importance 8/10、access 0：eff=90×(1+3.0×0.8)=306，retention=exp(-90/306)≈0.745 > 0.3 → 保留
    MemoryEntry core;
    core.type = MemoryType::Core;
    core.key = QStringLiteral("user:identity");
    core.summary = QStringLiteral("用户身份 gamma");
    core.content = core.summary;
    core.importance = 0.5;  // 故意低，验证 addEntry 的 Core importance 下限保护
    core.strength = 0.5;
    core.updatedAt = QDateTime::currentDateTimeUtc().addDays(-90);
    core.lastAccessedAt = core.updatedAt;
    const MemoryEntry storedCore = store.addEntry(core);
    QVERIFY(storedCore.importance >= 0.8);   // Core 类型 importance 下限生效
    QCOMPARE(storedCore.partition, QStringLiteral("semantic"));

    MemoryOrganizeTool tool(&store);
    QJsonObject params;
    params[QStringLiteral("mode")] = QStringLiteral("forget");
    const ToolResult result = tool.execute(params);
    QVERIFY(result.success);

    const QJsonObject stats = result.data;
    QVERIFY(stats.value(QStringLiteral("forgotten")).toInt() >= 1);

    // 重新确认状态：stale → Expired，identity 仍 Active；条目仍在（未物理删除）
    const auto all = store.all();
    bool staleExpired = false;
    bool coreActive = false;
    for (const MemoryEntry& e : all) {
        if (e.key == QStringLiteral("stale:event")) {
            QCOMPARE(e.status, MemoryStatus::Expired);
            staleExpired = true;
        }
        if (e.key == QStringLiteral("user:identity")) {
            QCOMPARE(e.status, MemoryStatus::Active);
            coreActive = true;
        }
    }
    QVERIFY(staleExpired);
    QVERIFY(coreActive);
}

// 测试用确定性 EmbeddingProvider：16 维，token 哈希到维度上。无真实模型也能验证
// upsert/search/remove 与余弦检索的链路。语义近似由共享 token 体现。
class FakeEmbeddingProvider : public EmbeddingProvider {
public:
    QString modelName() const override { return QStringLiteral("fake-16d"); }
    int dimension() const override { return 16; }
    QVector<float> embed(const QString& text) override {
        QVector<float> v(16, 0.0f);
        const QStringList tokens = text.toLower().split(QRegularExpression(QStringLiteral("\\W+")),
                                                       Qt::SkipEmptyParts);
        for (const QString& tok : tokens) {
            int bucket = 0;
            for (const QChar& c : tok) bucket = (bucket * 31 + c.unicode()) % 16;
            v[bucket] += 1.0f;
        }
        return v;
    }
};

class CountingEmbeddingProvider : public FakeEmbeddingProvider {
public:
    int calls = 0;
    QVector<float> embed(const QString& text) override {
        ++calls;
        return FakeEmbeddingProvider::embed(text);
    }
};

// SqliteEmbeddingIndex：upsert 写向量到 memory_embeddings，search 余弦 top-k，
// remove 删行。复用 MemoryStore 同一 DB 连接。注入 retriever 后语义命中排名前列。
void TestMemoryStrategy::testSqliteEmbeddingIndexSearch() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry cpp;
    cpp.type = MemoryType::Semantic;
    cpp.key = QStringLiteral("lang:cpp");
    cpp.summary = QStringLiteral("用户喜欢 c++ 编程语言");
    const MemoryEntry storedCpp = store.addEntry(cpp);

    MemoryEntry java;
    java.type = MemoryType::Semantic;
    java.key = QStringLiteral("lang:java");
    java.summary = QStringLiteral("用户偶尔写 java 后端");
    const MemoryEntry storedJava = store.addEntry(java);

    FakeEmbeddingProvider provider;
    SqliteEmbeddingIndex index(store.databaseConnectionName(), &provider);

    // upsert：cpp 用 c++ 文本，java 用 java 文本
    QVERIFY(index.upsert(storedCpp.id, QStringLiteral("c++ programming language")));
    QVERIFY(index.upsert(storedJava.id, QStringLiteral("java backend server")));

    // 查询 "c++" 应排到含 c++ token 的记忆附近（fake 向量下两者含 cpp 维度）
    const QList<EmbeddingSearchResult> hits = index.search(QStringLiteral("c++"), 5);
    QVERIFY(!hits.isEmpty());
    QVERIFY(hits.first().similarity > 0.0);

    // remove 后该 id 不再出现在结果中
    const QString removedId = hits.first().memoryId;
    QVERIFY(index.remove(removedId));
    const QList<EmbeddingSearchResult> after = index.search(QStringLiteral("c++"), 5);
    for (const EmbeddingSearchResult& r : after) {
        QVERIFY(r.memoryId != removedId);
    }

    // 注入 retriever：embedding 通道产出候选且打 "embedding" reason。
// 用与记忆正文不重叠的 query token（"rust"），使 Phase1 关键词关闸（return 0）→
// 直接候选为空 → embedding 候选进入，带 "embedding" reason。
MemoryRetriever retriever;
    MemoryQuery query;
    query.text = QStringLiteral("rust");
    query.limit = 5;
    const QList<RetrievedMemory> withIndex = retriever.retrieve(store, query, nullptr, &index);
    bool hasEmbeddingReason = false;
    for (const RetrievedMemory& m : withIndex) {
        if (m.reasons.contains(QStringLiteral("embedding"))) hasEmbeddingReason = true;
    }
    QVERIFY(hasEmbeddingReason);
}

// HNSW：验证 ANN 查询、稳定 label、墓碑删除和文件重载。
void TestMemoryStrategy::testHnswEmbeddingIndexSearchAndPersistence() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    FakeEmbeddingProvider provider;
    HnswIndexParams params;
    params.initialCapacity = 32;
    HnswEmbeddingIndex index(store.databaseConnectionName(), &provider, tempDir.path(), params);
    QVERIFY(index.upsert(QStringLiteral("hnsw-a"), QStringLiteral("alpha shared topic")));
    QVERIFY(index.upsert(QStringLiteral("hnsw-b"), QStringLiteral("beta unrelated topic")));
    const QList<EmbeddingSearchResult> first = index.search(QStringLiteral("alpha"), 2);
    QVERIFY(!first.isEmpty());
    QCOMPARE(first.first().memoryId, QStringLiteral("hnsw-a"));
    QVERIFY(index.upsert(QStringLiteral("hnsw-a"), QStringLiteral("alpha shared topic")));
    QCOMPARE(index.activeCount(), 2);
    QVERIFY(index.remove(QStringLiteral("hnsw-a")));
    for (const auto& hit : index.search(QStringLiteral("alpha"), 5))
        QVERIFY(hit.memoryId != QStringLiteral("hnsw-a"));
    QVERIFY(index.saveToDisk());

    HnswEmbeddingIndex restored(store.databaseConnectionName(), &provider, tempDir.path(), params);
    QVERIFY(restored.loadFromDisk());
    QCOMPARE(restored.activeCount(), 1);
    QVERIFY(restored.search(QStringLiteral("beta"), 1).first().memoryId == QStringLiteral("hnsw-b"));
}

void TestMemoryStrategy::testMemoryIndexWorkerProcessesOutbox() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    MemoryStore store;
    setupStoreWithDb(store, tempDir);
    FakeEmbeddingProvider provider;
    HnswEmbeddingIndex index(store.databaseConnectionName(), &provider, tempDir.path());

    MemoryEntry entry;
    entry.type = MemoryType::Semantic;
    entry.key = QStringLiteral("worker:test");
    entry.summary = QStringLiteral("worker indexed alpha");
    const MemoryEntry stored = store.addEntry(entry);
    QVERIFY(!stored.id.isEmpty());

    MemoryIndexWorker worker(index);
    QVERIFY(worker.processPending() >= 1);
    QVERIFY(!index.search(QStringLiteral("alpha"), 1).isEmpty());

    QSqlDatabase db = QSqlDatabase::database(store.databaseConnectionName(), false);
    QSqlQuery status(db);
    status.prepare(QStringLiteral("SELECT COUNT(*) FROM memory_index_jobs WHERE memory_id=:id AND status='Completed'"));
    status.bindValue(QStringLiteral(":id"), stored.id);
    QVERIFY(status.exec());
    QVERIFY(status.next());
    QVERIFY(status.value(0).toInt() >= 1);
}

void TestMemoryStrategy::testMemoryIndexWorkerDurableCompletion() {
    QTemporaryDir dir;
    MemoryStore store;
    setupStoreWithDb(store, dir);
    FakeEmbeddingProvider provider;
    MemoryEntry entry;
    entry.type = MemoryType::Semantic;
    entry.summary = QStringLiteral("durable alpha");
    const auto stored = store.addEntry(entry);
    QVERIFY(!stored.id.isEmpty());
    const auto db = QSqlDatabase::database(store.databaseConnectionName(), false);
    QSqlQuery job(db);
    QVERIFY(job.exec(QStringLiteral("SELECT id FROM memory_index_jobs LIMIT 1")));
    QVERIFY(job.next());
    const QString id = job.value(0).toString();
    job.finish();
    {
        HnswEmbeddingIndex index(store.databaseConnectionName(), &provider, dir.path());
        MemoryIndexWorker worker(index);
        QVERIFY(worker.processOne(id));
        QVERIFY(QFile::exists(index.indexFilePath()));
        QSqlQuery vector(db);
        QVERIFY(vector.exec(QStringLiteral("SELECT COUNT(*) FROM memory_embeddings")));
        QVERIFY(vector.next());
        QCOMPARE(vector.value(0).toInt(), 1);
    }
    HnswEmbeddingIndex restored(store.databaseConnectionName(), &provider, dir.path());
    QVERIFY(restored.loadFromDisk());
    QCOMPARE(restored.activeCount(), 1);
    const auto hits = restored.search(QStringLiteral("alpha"), 1);
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.first().memoryId, stored.id);

    // Simulate a crash after file persistence but before the completion marker.
    QVERIFY(job.exec(QStringLiteral("UPDATE memory_index_jobs SET status='Processing'")));
    MemoryIndexWorker replay(restored);
    QCOMPARE(replay.processPending(), 1);
    QCOMPARE(restored.activeCount(), 1);
    QCOMPARE(restored.tombstoneCount(), 0);
    QCOMPARE(replay.processPending(), 0);
}

void TestMemoryStrategy::testMemoryIndexWorkerRetriesFailedSave() {
    QTemporaryDir dir;
    MemoryStore store;
    setupStoreWithDb(store, dir);
    FakeEmbeddingProvider provider;
    MemoryEntry entry;
    entry.type = MemoryType::Semantic;
    entry.summary = QStringLiteral("retry alpha");
    QVERIFY(!store.addEntry(entry).id.isEmpty());
    const QString blockedPath = dir.filePath(QStringLiteral("index-directory"));
    QFile blocker(blockedPath);
    QVERIFY(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    HnswEmbeddingIndex index(store.databaseConnectionName(), &provider, blockedPath);
    MemoryIndexWorker worker(index);
    QCOMPARE(worker.processPending(), 0);
    QSqlQuery status(QSqlDatabase::database(store.databaseConnectionName(), false));
    QVERIFY(status.exec(QStringLiteral("SELECT status,attempt_count FROM memory_index_jobs")));
    QVERIFY(status.next());
    QCOMPARE(status.value(0).toString(), QStringLiteral("Pending"));
    QCOMPARE(status.value(1).toInt(), 1);
    status.finish();
    QVERIFY(blocker.remove());
    QVERIFY(status.exec(QStringLiteral("UPDATE memory_index_jobs SET next_attempt_at=0")));
    QCOMPARE(worker.processPending(), 1);
    HnswEmbeddingIndex restored(store.databaseConnectionName(), &provider, blockedPath);
    QVERIFY(restored.loadFromDisk());
    QCOMPARE(restored.activeCount(), 1);
}

void TestMemoryStrategy::testMemoryIndexWorkerDeleteSurvivesRestart() {
    QTemporaryDir dir;
    MemoryStore store;
    setupStoreWithDb(store, dir);
    FakeEmbeddingProvider provider;
    MemoryEntry entry;
    entry.type = MemoryType::Semantic;
    entry.summary = QStringLiteral("delete alpha");
    const auto stored = store.addEntry(entry);
    QVERIFY(!stored.id.isEmpty());
    {
        HnswEmbeddingIndex index(store.databaseConnectionName(), &provider, dir.path());
        MemoryIndexWorker worker(index);
        QCOMPARE(worker.processPending(), 1);
    }
    QSqlQuery job(QSqlDatabase::database(store.databaseConnectionName(), false));
    QVERIFY(job.exec(QStringLiteral("UPDATE memory_index_jobs SET operation='delete',status='Processing'")));
    {
        HnswEmbeddingIndex restarted(store.databaseConnectionName(), &provider, dir.path());
        MemoryIndexWorker worker(restarted);
        QCOMPARE(worker.processPending(), 1);
        QCOMPARE(restarted.activeCount(), 1);
    }
    QVERIFY(job.exec(QStringLiteral("SELECT COUNT(*) FROM memory_embeddings")));
    QVERIFY(job.next());
    QCOMPARE(job.value(0).toInt(), 1);
    HnswEmbeddingIndex restored(store.databaseConnectionName(), &provider, dir.path());
    QVERIFY(restored.loadFromDisk());
    QCOMPARE(restored.activeCount(), 1);
    QCOMPARE(restored.tombstoneCount(), 0);
    QVERIFY(!restored.search(QStringLiteral("alpha"), 1).isEmpty());
}

void TestMemoryStrategy::testHnswRejectsMismatchedFiles() {
    QTemporaryDir dir;
    MemoryStore store;
    setupStoreWithDb(store, dir);
    FakeEmbeddingProvider provider;
    HnswEmbeddingIndex index(store.databaseConnectionName(), &provider, dir.path());
    QVERIFY(index.upsert(QStringLiteral("file-a"), QStringLiteral("alpha")));
    QVERIFY(index.saveToDisk());
    QFile metadata(index.metaFilePath());
    QVERIFY(metadata.open(QIODevice::ReadOnly));
    const QByteArray previousMetadata = metadata.readAll();
    metadata.close();
    QVERIFY(index.upsert(QStringLiteral("file-b"), QStringLiteral("beta")));
    QVERIFY(index.saveToDisk());
    QVERIFY(metadata.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(metadata.write(previousMetadata), previousMetadata.size());
    metadata.close();
    HnswEmbeddingIndex restored(store.databaseConnectionName(), &provider, dir.path());
    QVERIFY(!restored.loadFromDisk());
    QVERIFY(!restored.isReady());
    QVERIFY(index.saveToDisk());
    QVERIFY(restored.loadFromDisk());
    QFile binary(index.indexFilePath());
    QVERIFY(binary.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(binary.write("broken"), 6);
    binary.close();
    QVERIFY(!restored.loadFromDisk());
    QVERIFY(!restored.isReady());
    QVERIFY(restored.search(QStringLiteral("alpha"), 2).isEmpty());
}

void TestMemoryStrategy::testHnswUpdatesLabelsAndRebuildsFromAuthoritativeVectors() {
    QTemporaryDir dir;
    MemoryStore store;
    setupStoreWithDb(store, dir);
    FakeEmbeddingProvider provider;
    HnswEmbeddingIndex index(store.databaseConnectionName(), &provider, dir.path(), HnswIndexParams{16, 200, 50, 0.30, 1});
    MemoryEntry entry;
    entry.type = MemoryType::Semantic;
    entry.summary = QStringLiteral("alpha");
    const auto stored = store.addEntry(entry);
    QVERIFY(!stored.id.isEmpty());
    MemoryIndexWorker worker(index);
    QVERIFY(worker.processPending() >= 1);
    const auto first = index.search(QStringLiteral("alpha"), 1);
    QCOMPARE(first.size(), 1);
    QVERIFY(index.upsert(stored.id, QStringLiteral("beta")));
    QVERIFY(index.search(QStringLiteral("beta"), 1).first().memoryId == stored.id);
    QVERIFY(!index.search(QStringLiteral("alpha"), 1).isEmpty());
    QVERIFY(index.search(QStringLiteral("alpha"), 1).first().similarity <
            index.search(QStringLiteral("beta"), 1).first().similarity);

    QVERIFY(index.rebuildFromRepository());
    QVERIFY(index.search(QStringLiteral("alpha"), 1).first().memoryId == stored.id);
    QCOMPARE(index.activeCount(), 1);
    QVERIFY(index.tombstoneCount() == 0);

    for (int i = 0; i < 40; ++i) {
        const QString id = QStringLiteral("capacity-%1").arg(i);
        QVERIFY(index.upsert(id, QStringLiteral("capacity vector %1").arg(i)));
    }
    QCOMPARE(index.activeCount(), 41);
}

void TestMemoryStrategy::testIndexWorkerAutomaticRecovery_data() {
    QTest::addColumn<QString>("damage");
    QTest::addColumn<bool>("pending");
    for (const auto& damage : {QStringLiteral("binary"), QStringLiteral("metadata"), QStringLiteral("labels")}) {
        QTest::newRow(qPrintable(damage + "-empty-queue")) << damage << false;
        QTest::newRow(qPrintable(damage + "-pending")) << damage << true;
    }
}

void TestMemoryStrategy::testIndexWorkerAutomaticRecovery() {
    QFETCH(QString, damage);
    QFETCH(bool, pending);
    QTemporaryDir dir;
    MemoryStore store;
    setupStoreWithDb(store, dir);
    CountingEmbeddingProvider provider;
    HnswEmbeddingIndex initial(store.databaseConnectionName(), &provider, dir.path());
    for (const QString& id : {QStringLiteral("alpha"), QStringLiteral("beta")}) {
        MemoryEntry entry;
        entry.id = id;
        entry.summary = id;
        entry.type = MemoryType::Semantic;
        QVERIFY(!store.addEntry(entry).id.isEmpty());
    }
    MemoryIndexWorker worker(initial);
    QCOMPARE(worker.processPending(), 2);
    if (damage == "binary") {
        QFile binary(initial.indexFilePath());
        QVERIFY(binary.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(binary.write("broken"), 6);
    } else if (damage == "metadata") {
        QVERIFY(QFile::remove(initial.metaFilePath()));
    } else {
        QSqlQuery query(QSqlDatabase::database(store.databaseConnectionName(), false));
        QVERIFY(query.exec(QStringLiteral("DELETE FROM memory_hnsw_labels")));
    }
    if (pending) {
        MemoryEntry entry;
        entry.id = QStringLiteral("gamma");
        entry.type = MemoryType::Semantic;
        entry.summary = entry.id;
        QVERIFY(!store.addEntry(entry).id.isEmpty());
        QSqlQuery query(QSqlDatabase::database(store.databaseConnectionName(), false));
        QVERIFY(query.exec(QStringLiteral("UPDATE memory_index_jobs SET status='Processing' WHERE memory_id='gamma'")));
    }
    provider.calls = 0;
    HnswEmbeddingIndex recovered(store.databaseConnectionName(), &provider, dir.path());
    MemoryIndexWorker restart(recovered);
    QCOMPARE(restart.processPending(), pending ? 1 : 0);
    QVERIFY(recovered.isReady());
    QCOMPARE(recovered.activeCount(), pending ? 3 : 2);
    QCOMPARE(provider.calls, pending ? 1 : 0);
    HnswEmbeddingIndex reloaded(store.databaseConnectionName(), &provider, dir.path());
    QVERIFY(reloaded.loadFromDisk());
    const auto hits = reloaded.searchVector(FakeEmbeddingProvider().embed(QStringLiteral("alpha")), 8);
    QCOMPARE(hits.size(), pending ? 3 : 2);
    QCOMPARE(hits.first().memoryId, QStringLiteral("alpha"));
    QCOMPARE(restart.processPending(), 0);
}

void TestMemoryStrategy::testHnswRebuildFiltersInvalidRows() {
    QTemporaryDir dir;
    MemoryStore store;
    setupStoreWithDb(store, dir);
    CountingEmbeddingProvider provider;
    HnswEmbeddingIndex index(store.databaseConnectionName(), &provider, dir.path());
    for (int i = 0; i < 13; ++i) {
        MemoryEntry entry;
        entry.id = QString::number(i);
        entry.type = MemoryType::Semantic;
        entry.summary = QStringLiteral("alpha %1").arg(i);
        QVERIFY(!store.addEntry(entry).id.isEmpty());
    }
    MemoryIndexWorker worker(index);
    QCOMPARE(worker.processPending(), 13);
    QSqlQuery query(QSqlDatabase::database(store.databaseConnectionName(), false));
    QVERIFY(query.exec(QStringLiteral("UPDATE memory_items SET privacy_level='sensitive' WHERE id='1'")));
    QVERIFY(query.exec(QStringLiteral("UPDATE memory_items SET status='archived' WHERE id='2'")));
    QVERIFY(query.exec(QStringLiteral("UPDATE memory_items SET partition='hippocampus' WHERE id='3'")));
    QVERIFY(query.exec(QStringLiteral("UPDATE memory_items SET type='working' WHERE id='4'")));
    QVERIFY(query.exec(QStringLiteral("UPDATE memory_embeddings SET dimension=2,vector_blob=zeroblob(8) WHERE memory_id='5'")));
    QVERIFY(query.exec(QStringLiteral("UPDATE memory_embeddings SET vector_blob=zeroblob(64) WHERE memory_id='6'")));
    QVector<float> nanVector(16, std::numeric_limits<float>::quiet_NaN());
    query.prepare(QStringLiteral("UPDATE memory_embeddings SET vector_blob=:blob WHERE memory_id='7'"));
    query.bindValue(QStringLiteral(":blob"), QByteArray(reinterpret_cast<const char*>(nanVector.constData()), 64));
    QVERIFY(query.exec());
    QVERIFY(query.exec(QStringLiteral("UPDATE memory_embeddings SET vector_blob=X'00' WHERE memory_id='8'")));
    QVERIFY(query.exec(QStringLiteral("UPDATE memory_embeddings SET model='other-model' WHERE memory_id='9'")));
    QVERIFY(query.exec(QStringLiteral("UPDATE memory_items SET expires_at='2000-01-01T00:00:00Z' WHERE id='10'")));
    QVERIFY(query.exec(QStringLiteral("UPDATE memory_items SET type='short_term' WHERE id='11'")));
    nanVector.fill(std::numeric_limits<float>::infinity());
    query.prepare(QStringLiteral("UPDATE memory_embeddings SET vector_blob=:blob WHERE memory_id='12'"));
    query.bindValue(QStringLiteral(":blob"), QByteArray(reinterpret_cast<const char*>(nanVector.constData()), 64));
    QVERIFY(query.exec());
    provider.calls = 0;
    QVERIFY(index.rebuildFromRepository());
    QCOMPARE(provider.calls, 0);
    QCOMPARE(index.activeCount(), 1);
    QCOMPARE(index.tombstoneCount(), 0);
    HnswEmbeddingIndex restored(store.databaseConnectionName(), &provider, dir.path());
    QVERIFY(restored.loadFromDisk());
    const auto hits = restored.searchVector(FakeEmbeddingProvider().embed(QStringLiteral("alpha")), 16);
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.first().memoryId, QStringLiteral("0"));
    QVERIFY(query.exec(QStringLiteral("UPDATE memory_index_jobs SET status='Pending' WHERE memory_id IN ('1','2','3','4','10','11')")));
    MemoryIndexWorker replay(restored);
    QCOMPARE(replay.processPending(), 6);
    QCOMPARE(provider.calls, 0);
    QCOMPARE(restored.activeCount(), 1);
    QVERIFY(query.exec(QStringLiteral("DELETE FROM memory_embeddings WHERE memory_id='0'")));
    QVERIFY(restored.rebuildFromRepository());
    QCOMPARE(restored.activeCount(), 0);
    HnswEmbeddingIndex empty(store.databaseConnectionName(), &provider, dir.path());
    QVERIFY(empty.loadFromDisk());
    QCOMPARE(empty.activeCount(), 0);
}

void TestMemoryStrategy::testHnswRebuildFailureIsRetryable_data() {
    QTest::addColumn<QString>("failure");
    QTest::newRow("sql-read") << QStringLiteral("read");
    QTest::newRow("sql-write") << QStringLiteral("write");
    QTest::newRow("metadata-save") << QStringLiteral("save");
}

void TestMemoryStrategy::testHnswRebuildFailureIsRetryable() {
    QFETCH(QString, failure);
    QTemporaryDir dir;
    MemoryStore store;
    setupStoreWithDb(store, dir);
    CountingEmbeddingProvider provider;
    HnswEmbeddingIndex index(store.databaseConnectionName(), &provider, dir.path());
    for (const auto& id : {QStringLiteral("alpha"), QStringLiteral("beta")}) {
        MemoryEntry entry;
        entry.id = id;
        entry.type = MemoryType::Semantic;
        entry.summary = id;
        QVERIFY(!store.addEntry(entry).id.isEmpty());
    }
    MemoryIndexWorker worker(index);
    QCOMPARE(worker.processPending(), 2);
    QSqlQuery query(QSqlDatabase::database(store.databaseConnectionName(), false));
    QVERIFY(query.exec(QStringLiteral("DELETE FROM memory_embeddings WHERE memory_id='beta'")));
    if (failure == "read") {
        QVERIFY(query.exec(QStringLiteral("ALTER TABLE memory_embeddings RENAME TO saved_embeddings")));
    } else if (failure == "write") {
        QVERIFY(query.exec(QStringLiteral("CREATE TEMP TRIGGER reject_label BEFORE UPDATE ON memory_hnsw_labels "
                                          "WHEN NEW.status='Active' "
                                          "BEGIN SELECT RAISE(ABORT, 'injected label failure'); END")));
    } else {
        QVERIFY(QFile::remove(index.metaFilePath()));
        QVERIFY(QDir().mkdir(index.metaFilePath()));
    }
    QString error;
    QVERIFY(!index.rebuildFromRepository(&error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!index.isReady());
    QCOMPARE(index.activeCount(), 0);
    QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM memory_hnsw_labels WHERE status='Active'")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 2);
    query.finish();
    if (failure == "read") QVERIFY(query.exec(QStringLiteral("ALTER TABLE saved_embeddings RENAME TO memory_embeddings")));
    else if (failure == "write") QVERIFY(query.exec(QStringLiteral("DROP TRIGGER reject_label")));
    else QVERIFY(QDir().rmdir(index.metaFilePath()));
    provider.calls = 0;
    QVERIFY(index.rebuildFromRepository());
    QCOMPARE(provider.calls, 0);
    QCOMPARE(index.activeCount(), 1);
    HnswEmbeddingIndex restored(store.databaseConnectionName(), &provider, dir.path());
    QVERIFY(restored.loadFromDisk());
    QCOMPARE(restored.activeCount(), 1);
}

void TestMemoryStrategy::testIndexWorkerRecoveryFailureKeepsJobPending() {
    QTemporaryDir dir;
    MemoryStore store;
    setupStoreWithDb(store, dir);
    CountingEmbeddingProvider provider;
    HnswEmbeddingIndex initial(store.databaseConnectionName(), &provider, dir.path());
    MemoryEntry entry;
    entry.type = MemoryType::Semantic;
    entry.summary = QStringLiteral("alpha");
    entry.id = QStringLiteral("alpha");
    QVERIFY(!store.addEntry(entry).id.isEmpty());
    MemoryIndexWorker worker(initial);
    QCOMPARE(worker.processPending(), 1);
    QVERIFY(QFile::remove(initial.metaFilePath()));
    QVERIFY(QDir().mkdir(initial.metaFilePath()));
    entry.id = QStringLiteral("beta");
    entry.summary = entry.id;
    QVERIFY(!store.addEntry(entry).id.isEmpty());
    HnswEmbeddingIndex recovered(store.databaseConnectionName(), &provider, dir.path());
    MemoryIndexWorker restart(recovered);
    QCOMPARE(restart.processPending(), 0);
    QVERIFY(!recovered.isReady());
    QSqlQuery query(QSqlDatabase::database(store.databaseConnectionName(), false));
    QVERIFY(query.exec(QStringLiteral("SELECT status,attempt_count FROM memory_index_jobs WHERE memory_id='beta'")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), QStringLiteral("Pending"));
    QCOMPARE(query.value(1).toInt(), 1);
    QVERIFY(QDir().rmdir(initial.metaFilePath()));
    QVERIFY(query.exec(QStringLiteral("UPDATE memory_index_jobs SET next_attempt_at=0 WHERE memory_id='beta'")));
    QCOMPARE(restart.processPending(), 1);
    QCOMPARE(recovered.activeCount(), 2);
    HnswEmbeddingIndex restored(store.databaseConnectionName(), &provider, dir.path());
    QVERIFY(restored.loadFromDisk());
    QCOMPARE(restored.activeCount(), 2);
}

void TestMemoryStrategy::testHnswReinsertAndInvalidVectors() {
    QTemporaryDir dir;
    MemoryStore store;
    setupStoreWithDb(store, dir);
    FakeEmbeddingProvider provider;
    HnswEmbeddingIndex index(store.databaseConnectionName(), &provider, dir.path());
    QVERIFY(index.upsert(QStringLiteral("alpha"), QStringLiteral("alpha")));
    QVERIFY(index.remove(QStringLiteral("alpha")));
    QCOMPARE(index.tombstoneCount(), 1);
    QVERIFY(index.upsert(QStringLiteral("alpha"), QStringLiteral("alpha")));
    QCOMPARE(index.activeCount(), 1);
    QCOMPARE(index.tombstoneCount(), 0);
    QVERIFY(!index.upsertVector(QStringLiteral("wrong-dim"), {1, 0}, QStringLiteral("hash")));
    QVERIFY(!index.upsertVector(QStringLiteral("zero"), QVector<float>(16, 0.0f), QStringLiteral("hash")));
    QVERIFY(!index.upsertVector(QStringLiteral("nan"), QVector<float>(16, std::numeric_limits<float>::quiet_NaN()), QStringLiteral("hash")));
    QCOMPARE(index.activeCount(), 1);
    QVERIFY(index.saveToDisk());
    HnswEmbeddingIndex restored(store.databaseConnectionName(), &provider, dir.path());
    QVERIFY(restored.loadFromDisk());
    QCOMPARE(restored.activeCount(), 1);
}

void TestMemoryStrategy::testIndexJobsReconcileCurrentState() {
    QTemporaryDir dir;
    MemoryStore store;
    setupStoreWithDb(store, dir);
    CountingEmbeddingProvider provider;
    HnswEmbeddingIndex index(store.databaseConnectionName(), &provider, dir.path());
    MemoryEntry entry;
    entry.id = QStringLiteral("versioned");
    entry.type = MemoryType::Semantic;
    entry.summary = QStringLiteral("alpha");
    entry = store.addEntry(entry);
    QVERIFY(!entry.id.isEmpty());
    MemoryIndexWorker worker(index);
    QCOMPARE(worker.processPending(), 1);
    QVERIFY(store.updateStatusById(entry.id, MemoryStatus::Archived));
    QSqlQuery query(QSqlDatabase::database(store.databaseConnectionName(), false));
    QVERIFY(query.exec(QStringLiteral("SELECT id FROM memory_index_jobs WHERE operation='delete'")));
    QVERIFY(query.next());
    const QString oldDelete = query.value(0).toString();
    query.finish();
    entry.summary = QStringLiteral("beta");
    entry.content = entry.summary;
    QVERIFY(store.updateEntryById(entry));
    QCOMPARE(worker.processPending(), 2);
    QVERIFY(query.exec(QStringLiteral("UPDATE memory_index_jobs SET status='Processing' WHERE operation='delete'")));
    QVERIFY(worker.processOne(oldDelete));
    QCOMPARE(index.activeCount(), 1);
    const auto hits = index.search(QStringLiteral("beta"), 1);
    QCOMPARE(hits.size(), 1);
    QVERIFY(hits.first().similarity > 0.99);
    QVERIFY(query.exec(QStringLiteral("SELECT content_hash,model FROM memory_index_jobs WHERE id='" ) + oldDelete + "'"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), HnswEmbeddingIndex::contentHash(QStringLiteral("beta\nbeta")));
    QCOMPARE(query.value(1).toString(), provider.modelName());
    query.finish();
    provider.calls = 0;
    QVERIFY(query.exec(QStringLiteral("UPDATE memory_index_jobs SET status='Processing'")));
    QCOMPARE(worker.processPending(), 3);
    QCOMPARE(provider.calls, 1); // Only the new beta content requires inference.
    QCOMPARE(index.activeCount(), 1);
}

void TestMemoryStrategy::testIndexJobsRespectModelBinding() {
    QTemporaryDir dir;
    MemoryStore store;
    setupStoreWithDb(store, dir);
    CountingEmbeddingProvider provider;
    MemoryEntry entry;
    entry.type = MemoryType::Semantic;
    entry.summary = QStringLiteral("alpha");
    QVERIFY(!store.addEntry(entry).id.isEmpty());
    QSqlQuery query(QSqlDatabase::database(store.databaseConnectionName(), false));
    QVERIFY(query.exec(QStringLiteral("UPDATE memory_index_jobs SET model='other-model-v2'")));
    HnswEmbeddingIndex index(store.databaseConnectionName(), &provider, dir.path());
    MemoryIndexWorker worker(index);
    QCOMPARE(worker.processPending(), 0);
    QCOMPARE(provider.calls, 0);
    QVERIFY(query.exec(QStringLiteral("SELECT id,status,attempt_count FROM memory_index_jobs")));
    QVERIFY(query.next());
    const auto id = query.value(0).toString();
    QCOMPARE(query.value(1).toString(), QStringLiteral("Pending"));
    QCOMPARE(query.value(2).toInt(), 0);
    query.finish();
    QVERIFY(!worker.processOne(id));
}

void TestMemoryStrategy::testIndexJobsRejectChangesDuringEmbedding() {
    QTemporaryDir dir;
    MemoryStore store;
    setupStoreWithDb(store, dir);
    class ChangingProvider : public FakeEmbeddingProvider {
    public:
        std::function<void()> change;
        QVector<float> embed(const QString& text) override {
            const auto vector = FakeEmbeddingProvider::embed(text);
            if (change) { auto callback = std::move(change); change = {}; callback(); }
            return vector;
        }
    } provider;
    MemoryEntry entry;
    entry.type = MemoryType::Semantic;
    entry.summary = QStringLiteral("alpha");
    entry = store.addEntry(entry);
    QVERIFY(!entry.id.isEmpty());
    bool mutated = false;
    provider.change = [&]() {
        entry.privacyLevel = PrivacyLevel::Sensitive;
        mutated = store.updateEntryById(entry);
    };
    HnswEmbeddingIndex index(store.databaseConnectionName(), &provider, dir.path());
    MemoryIndexWorker worker(index);
    QCOMPARE(worker.processPending(), 0);
    QVERIFY(mutated);
    QCOMPARE(index.activeCount(), 0);
    QSqlQuery query(QSqlDatabase::database(store.databaseConnectionName(), false));
    QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM memory_embeddings")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 0);
    query.finish();
    QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM memory_index_jobs WHERE status='Completed'")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 0);
}

void TestMemoryStrategy::testIndexJobsPersistBackoff() {
    QTemporaryDir dir;
    MemoryStore store;
    setupStoreWithDb(store, dir);
    class FailingProvider : public FakeEmbeddingProvider {
    public:
        int failures = 0;
        QVector<float> embed(const QString& text) override {
            if (text.startsWith(QStringLiteral("bad"))) { ++failures; return {}; }
            return FakeEmbeddingProvider::embed(text);
        }
    } provider;
    for (const auto& id : {QStringLiteral("bad"), QStringLiteral("good")}) {
        MemoryEntry entry;
        entry.id = id;
        entry.summary = id;
        entry.type = MemoryType::Semantic;
        QVERIFY(!store.addEntry(entry).id.isEmpty());
    }
    HnswEmbeddingIndex index(store.databaseConnectionName(), &provider, dir.path());
    MemoryIndexWorker worker(index);
    QCOMPARE(worker.processPending(), 1); // A failed item must not starve other work.
    QCOMPARE(provider.failures, 1);
    QSqlQuery query(QSqlDatabase::database(store.databaseConnectionName(), false));
    QVERIFY(query.exec(QStringLiteral("SELECT id,next_attempt_at,attempt_count FROM memory_index_jobs WHERE memory_id='bad'")));
    QVERIFY(query.next());
    const QString failedId = query.value(0).toString();
    QVERIFY(query.value(1).toLongLong() > QDateTime::currentMSecsSinceEpoch());
    QCOMPARE(query.value(2).toInt(), 1);
    query.finish();
    HnswEmbeddingIndex reloaded(store.databaseConnectionName(), &provider, dir.path());
    MemoryIndexWorker restart(reloaded);
    QCOMPARE(restart.processPending(), 0);
    QVERIFY(!restart.processOne(failedId));
    QCOMPARE(provider.failures, 1);
    QVERIFY(query.exec(QStringLiteral("UPDATE memory_index_jobs SET next_attempt_at=0 WHERE memory_id='bad'")));
    QCOMPARE(restart.processPending(), 0);
    QCOMPARE(provider.failures, 2);
    QVERIFY(query.exec(QStringLiteral("SELECT attempt_count,next_attempt_at FROM memory_index_jobs WHERE memory_id='bad'")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 2);
    QVERIFY(query.value(1).toLongLong() >= QDateTime::currentMSecsSinceEpoch() + 8000);
}

// SemanticIndexService：无 provider 禁用；有 provider 时空闲 tick 小批量消费并可召回。
void TestMemoryStrategy::testSemanticIndexServiceLifecycle() {
    QTemporaryDir dir;
    MemoryStore store;
    setupStoreWithDb(store, dir);

    SemanticIndexService disabled;
    QVERIFY(!disabled.start(nullptr, store.databaseConnectionName(), dir.path()));
    QVERIFY(!disabled.isEnabled());
    QVERIFY(disabled.index() == nullptr);
    QCOMPARE(disabled.runOnce(), 0);

    for (const auto& id : {QStringLiteral("alpha"), QStringLiteral("beta"), QStringLiteral("gamma")}) {
        MemoryEntry entry;
        entry.id = id;
        entry.type = MemoryType::Semantic;
        entry.summary = id;
        QVERIFY(!store.addEntry(entry).id.isEmpty());
    }

    SemanticIndexService service;
    service.setBatchSize(2);
    bool busy = true;
    service.setIdlePredicate([&busy]() { return !busy; });
    QVERIFY(service.start(std::make_unique<FakeEmbeddingProvider>(), store.databaseConnectionName(), dir.path()));
    QVERIFY(service.isEnabled());
    QVERIFY(service.index() != nullptr);
    QCOMPARE(service.runOnce(), 0);           // 忙碌时跳过
    busy = false;
    QCOMPARE(service.runOnce(), 2);           // 小批量
    QCOMPARE(service.runOnce(), 1);
    QCOMPARE(service.runOnce(), 0);
    QCOMPARE(service.processedTotal(), 3);
    const auto hits = service.index()->search(QStringLiteral("alpha"), 1);
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.first().memoryId, QStringLiteral("alpha"));

    // kick() 走事件循环：新任务在 tick 后被消化。
    MemoryEntry late;
    late.id = QStringLiteral("delta");
    late.type = MemoryType::Semantic;
    late.summary = late.id;
    QVERIFY(!store.addEntry(late).id.isEmpty());
    service.kick();
    QTRY_COMPARE(service.processedTotal(), 4);
    QCOMPARE(service.hnswIndex()->activeCount(), 4);
}

// 模型下载器：用本地 file:// 镜像验证下载/跳过/sha 校验，不依赖外网 HF.
// 在临时"源仓库"里按 HF 布局 repo/resolve/rev/file 摆好测试文件，镜像 host 指向它。
void TestMemoryStrategy::testModelDownloaderLocalMirror() {
    QTemporaryDir srcDir;
    QTemporaryDir destDir;
    QVERIFY(srcDir.isValid());
    QVERIFY(destDir.isValid());

    // 构造源文件 repo/resolve/main/model.txt（内容固定）
    const QString repo = QStringLiteral("BAAI/test-model");
    const QString revTree = repo + QStringLiteral("/resolve/main/");
    const QString modelRel = QStringLiteral("model.txt");
    const QString srcFile = srcDir.filePath(revTree + modelRel);
    QDir().mkpath(QFileInfo(srcFile).absolutePath());
    const QByteArray content = "hello embedding model";
    {
        QFile f(srcFile);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(content);
        f.close();
    }
    const QString sha = QString::fromLatin1(
        QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());

    ModelDownloader downloader;
    // 用本地 file:// 目录当镜像 host（注意末尾不带斜杠，buildUrl 会补）
    downloader.setMirrors({QUrl::fromLocalFile(srcDir.path()).toString()});
    downloader.setRevision(QStringLiteral("main"));
    downloader.setRetriesPerMirror(1);
    downloader.setTransferTimeoutMs(5000);

    // 第一次下载 + sha 校验通过
    ModelDownloader::FileSpec spec{modelRel, sha};
    QString err;
    QVERIFY(downloader.downloadSync(repo, destDir.path(), {spec}, &err));
    QFile downloaded(destDir.filePath(modelRel));
    QVERIFY(downloaded.exists());
    QCOMPARE(downloaded.open(QIODevice::ReadOnly) ? downloaded.readAll() : QByteArray(), content);

    // 第二次：文件已存在且 sha 通过 → 跳过（不再触碰源；把源删掉也该成功）
    QVERIFY(downloader.downloadSync(repo, destDir.path(), {spec}, &err));

    // sha 不匹配 → 应判定失败（校验失败会删文件，无其它镜像 → 整体失败）
    ModelDownloader::FileSpec badSpec{modelRel, QStringLiteral("0000")};
    QVERIFY(!downloader.downloadSync(repo, destDir.path(), {badSpec}, &err));

    ModelDownloader::FileSpec traversalSpec{QStringLiteral("../escaped.txt"), {}};
    QVERIFY(!downloader.downloadSync(repo, destDir.path(), {traversalSpec}, &err));
    QVERIFY(!QFileInfo::exists(QDir(destDir.path()).absoluteFilePath("../escaped.txt")));

    QTemporaryDir limitedDestDir;
    QVERIFY(limitedDestDir.isValid());
    ModelDownloader limitedDownloader;
    limitedDownloader.setMirrors({QUrl::fromLocalFile(srcDir.path()).toString()});
    limitedDownloader.setRevision(QStringLiteral("main"));
    limitedDownloader.setRetriesPerMirror(0);
    limitedDownloader.setTransferTimeoutMs(5000);
    limitedDownloader.setMaxFileBytes(8);
    QVERIFY(!limitedDownloader.downloadSync(repo, limitedDestDir.path(), {spec}, &err));
    QVERIFY(!QFileInfo::exists(limitedDestDir.filePath(modelRel)));

    QTemporaryDir outsideDestDir;
    QVERIFY(outsideDestDir.isValid());
    QFile outsideFile(outsideDestDir.filePath(QStringLiteral("external.txt")));
    QVERIFY(outsideFile.open(QIODevice::WriteOnly));
    outsideFile.write("outside model");
    outsideFile.close();

    const QString linkedDirectory = destDir.filePath(QStringLiteral("linked"));
    std::error_code linkError;
    std::filesystem::create_directory_symlink(
        std::filesystem::u8path(outsideDestDir.path().toUtf8().constData()),
        std::filesystem::u8path(linkedDirectory.toUtf8().constData()),
        linkError);
    if (!linkError) {
        ModelDownloader::FileSpec linkedSpec{QStringLiteral("linked/external.txt"), {}};
        QVERIFY(!downloader.downloadSync(repo, destDir.path(), {linkedSpec}, &err));
    }
}

// Daydream 地基：事务 ROLLBACK 必须撤销同一连接上的所有写入。
void TestMemoryStrategy::testTransactionRollbackRevertsWrites() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    QVERIFY(store.beginTransaction());

    const MemoryEntry written = store.add(MemoryType::Semantic,
                                          QStringLiteral("txn_rollback"),
                                          QStringLiteral("应被回滚的条目"),
                                          {QStringLiteral("txn")});
    QVERIFY(!written.id.isEmpty());
    QCOMPARE(store.all().size(), 1); // 内存镜像已更新

    QVERIFY(store.rollbackTransaction());

    // ROLLBACK 只撤 SQLite；内存镜像不丢，重 load 校验落盘真相
    QVERIFY(store.load());
    QCOMPARE(store.all().size(), 0);
    QVERIFY(!store.findById(written.id));
}

void TestMemoryStrategy::testTransactionCommitRetainsWrites() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    QVERIFY(store.beginTransaction());
    const MemoryEntry written = store.add(MemoryType::Semantic,
                                          QStringLiteral("txn_commit"),
                                          QStringLiteral("应保留的条目"),
                                          {QStringLiteral("txn")});
    QVERIFY(store.commitTransaction());

    QVERIFY(store.load());
    QCOMPARE(store.all().size(), 1);
    QVERIFY(store.findById(written.id));
}

void TestMemoryStrategy::testNestedTransactionsKeepOutboxAtomic() {
    QTemporaryDir dir;
    MemoryStore store;
    setupStoreWithDb(store, dir);
    const auto db = QSqlDatabase::database(store.databaseConnectionName(), false);
    auto count = [&db](const QString& table) {
        QSqlQuery query(db);
        if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM ") + table) || !query.next()) return -1;
        return query.value(0).toInt();
    };
    QVERIFY(store.beginTransaction());
    const auto first = store.add(MemoryType::Semantic, QStringLiteral("outer"), QStringLiteral("alpha"));
    QVERIFY(!first.id.isEmpty());
    QVERIFY(store.beginTransaction());
    QVERIFY(!store.add(MemoryType::Semantic, QStringLiteral("inner"), QStringLiteral("beta")).id.isEmpty());
    QCOMPARE(count(QStringLiteral("memory_index_jobs")), 2);
    QVERIFY(store.rollbackTransaction());
    QCOMPARE(count(QStringLiteral("memory_items")), 1);
    QCOMPARE(count(QStringLiteral("memory_index_jobs")), 1);
    QVERIFY(store.commitTransaction());
    QVERIFY(store.loadDatabaseOnly());
    QCOMPARE(store.all().size(), 1);
    QCOMPARE(store.all().first().id, first.id);

    QVERIFY(store.beginTransaction());
    const auto second = store.add(MemoryType::Semantic, QStringLiteral("rollback"), QStringLiteral("gamma"));
    QVERIFY(!second.id.isEmpty());
    MemoryRelation relation;
    relation.fromMemoryId = first.id;
    relation.toMemoryId = second.id;
    relation.type = MemoryRelationType::Related;
    QVERIFY(store.relationGraph().addRelation(relation));
    QVERIFY(store.tagCooccurrenceGraph().recordTags({QStringLiteral("alpha"), QStringLiteral("gamma")}));
    QCOMPARE(count(QStringLiteral("memory_index_jobs")), 2);
    QVERIFY(store.rollbackTransaction());
    QCOMPARE(count(QStringLiteral("memory_items")), 1);
    QCOMPARE(count(QStringLiteral("memory_index_jobs")), 1);
    QVERIFY(!store.relationGraph().hasRelation(first.id, second.id, MemoryRelationType::Related));
    QCOMPARE(store.tagCooccurrenceGraph().weightBetween(QStringLiteral("alpha"), QStringLiteral("gamma")), 0);
    QVERIFY(!store.commitTransaction());
    QVERIFY(!store.rollbackTransaction());
}

void TestMemoryStrategy::testNestedOutboxFailurePreservesOuterTransaction() {
    QTemporaryDir dir;
    MemoryStore store;
    setupStoreWithDb(store, dir);
    QVERIFY(store.beginTransaction());
    QVERIFY(!store.add(MemoryType::Semantic, QStringLiteral("before"), QStringLiteral("alpha")).id.isEmpty());
    QSqlQuery query(QSqlDatabase::database(store.databaseConnectionName(), false));
    QVERIFY(query.exec(QStringLiteral("CREATE TEMP TRIGGER fail_index_job BEFORE INSERT ON memory_index_jobs "
                                      "BEGIN SELECT RAISE(ABORT, 'injected outbox failure'); END")));
    QVERIFY(store.add(MemoryType::Semantic, QStringLiteral("failed"), QStringLiteral("beta")).id.isEmpty());
    QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM memory_items")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 1);
    query.finish();
    QVERIFY(query.exec(QStringLiteral("DROP TRIGGER fail_index_job")));
    QVERIFY(!store.add(MemoryType::Semantic, QStringLiteral("after"), QStringLiteral("gamma")).id.isEmpty());
    QVERIFY(store.commitTransaction());
    QVERIFY(store.loadDatabaseOnly());
    QCOMPARE(store.all().size(), 2);
    QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM memory_index_jobs")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 2);
}

void TestMemoryStrategy::testRepositoryTransactionsRespectExternalTransaction() {
    QTemporaryDir dir;
    SQLiteMemoryRepository repository;
    QVERIFY(repository.open(dir.filePath(QStringLiteral("external.sqlite"))));
    auto db = QSqlDatabase::database(repository.connectionName(), false);
    QVERIFY(db.transaction());
    QVERIFY(repository.beginTransaction());
    MemoryEntry entry;
    entry.id = QStringLiteral("external-entry");
    entry.type = MemoryType::Semantic;
    entry.partition = QStringLiteral("semantic");
    entry.summary = QStringLiteral("alpha");
    QVERIFY(repository.insert(entry));
    QVERIFY(repository.commitTransaction());
    QVERIFY(db.rollback());
    QCOMPARE(repository.loadAll().size(), 0);
    QVERIFY(repository.beginTransaction());
    db = QSqlDatabase();
    repository.close();
    QVERIFY(repository.open(dir.filePath(QStringLiteral("external.sqlite"))));
    QVERIFY(!repository.commitTransaction());
    QVERIFY(repository.beginTransaction());
    QVERIFY(repository.insert(entry));
    QVERIFY(repository.commitTransaction());
    QCOMPARE(repository.loadAll().size(), 1);
}

// ROLLBACK 也要撤销复用同一连接的 MemoryRelationGraph 写入（图残留防护）。
void TestMemoryStrategy::testTransactionRollbackRevertsRelationGraph() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    const MemoryEntry a = store.addEntry(store.add(MemoryType::Semantic, QStringLiteral("a"), QStringLiteral("a"), {}));
    const MemoryEntry b = store.addEntry(store.add(MemoryType::Semantic, QStringLiteral("b"), QStringLiteral("b"), {}));
    QVERIFY(store.load());
    const QString aId = store.all().at(0).id;
    const QString bId = store.all().at(1).id;

    QVERIFY(store.beginTransaction());
    MemoryRelationGraph& graph = store.relationGraph();
    MemoryRelation rel;
    rel.fromMemoryId = aId;
    rel.toMemoryId = bId;
    rel.type = MemoryRelationType::Related;
    QVERIFY(graph.addRelation(rel));
    QVERIFY(graph.hasRelation(aId, bId, MemoryRelationType::Related));

    QVERIFY(store.rollbackTransaction());

    MemoryStore reloaded;
    setupStoreWithDb(reloaded, tempDir);
    QVERIFY(!reloaded.relationGraph().hasRelation(aId, bId, MemoryRelationType::Related));
}

void TestMemoryStrategy::testTransactionRollbackRevertsTagCooccurrence() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);
    QVERIFY(store.beginTransaction());
    QVERIFY(store.tagCooccurrenceGraph().recordTags(
        {QStringLiteral("Qt"), QStringLiteral("C++")}));
    QCOMPARE(store.tagCooccurrenceGraph().weightBetween(
        QStringLiteral("qt"), QStringLiteral("c++")), 1);
    QVERIFY(store.rollbackTransaction());

    MemoryStore reloaded;
    setupStoreWithDb(reloaded, tempDir);
    QCOMPARE(reloaded.tagCooccurrenceGraph().weightBetween(
        QStringLiteral("qt"), QStringLiteral("c++")), 0);
}

// Daydream 第③步：硬编码降级巩固回路。mentionCount>=2 的 Hippocampus 条目应升级为
// Episodic 长期记忆并清空源；低价值条目应被丢弃清空 inbox；其他分区条目不受影响。
void TestMemoryStrategy::testDaydreamDrainUpgradesAndClearsHippocampus() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    // 一条高提及 Hippocampus 条目（ShortTerm→hippocampus），应被升级。
    MemoryEntry hot;
    hot.type = MemoryType::ShortTerm;
    hot.key = QStringLiteral("hot_topic");
    hot.summary = QStringLiteral("反复提到的面试安排");
    hot.content = hot.summary;
    hot.source = QStringLiteral("user_interaction");
    hot.importance = 0.4;
    hot.mentionCount = 2; // Three mentions select Semantic in the fallback policy.
    const QString hotId = store.addEntry(hot).id;
    QVERIFY(store.load());

    DaydreamConsolidator consolidator(store);
    const DaydreamConsolidator::Stats stats = consolidator.runHardcodedDrain();
    QVERIFY(stats.committed);
    QCOMPARE(stats.scanned, 1);
    QCOMPARE(stats.upgraded, 1);
    QCOMPARE(stats.discarded, 0);

    // 重读落盘真相：Hippocampus 清空，Episodic 多一条升级记忆。
    QVERIFY(store.load());
    bool hippocampusEmpty = true;
    int episodicCount = 0;
    for (const MemoryEntry& e : store.all()) {
        if (e.partition == QLatin1String("hippocampus")) hippocampusEmpty = false;
        if (e.type == MemoryType::Episodic) ++episodicCount;
    }
    QVERIFY(hippocampusEmpty);
    QCOMPARE(episodicCount, 1);
    QVERIFY(!store.findById(hotId)); // 源条目已物理删除
    QCOMPARE(store.all().first().sourceMemoryIds, QStringList{hotId});
}

void TestMemoryStrategy::testDaydreamOutboxFailureRollsBackBatch() {
    QTemporaryDir dir;
    MemoryStore store;
    setupStoreWithDb(store, dir);
    for (const QString& key : {QStringLiteral("first"), QStringLiteral("second")}) {
        MemoryEntry source;
        source.type = MemoryType::ShortTerm;
        source.key = key;
        source.summary = key;
        source.source = QStringLiteral("user_interaction");
        source.tags = {QStringLiteral("alpha"), QStringLiteral("beta")};
        source.mentionCount = 2;
        QVERIFY(!store.addEntry(source).id.isEmpty());
    }
    DaydreamConsolidator consolidator(store);
    const auto snapshot = consolidator.createSnapshot();
    QCOMPARE(snapshot.size(), 2);
    const auto decisions = DaydreamConsolidator::hardcodedDecisions(snapshot.items);
    QSqlQuery query(QSqlDatabase::database(store.databaseConnectionName(), false));
    QVERIFY(query.exec(QStringLiteral(
        "CREATE TEMP TRIGGER fail_second_long_term BEFORE INSERT ON memory_index_jobs "
        "WHEN (SELECT COUNT(*) FROM memory_items WHERE partition!='hippocampus')=2 "
        "BEGIN SELECT RAISE(ABORT, 'second result outbox failure'); END")));
    const auto failed = consolidator.applyDecisions(snapshot, decisions);
    QVERIFY(!failed.committed);
    QVERIFY(failed.failed > 0);
    QVERIFY(store.loadDatabaseOnly());
    QCOMPARE(store.all().size(), 2);
    for (const auto& source : snapshot.items) QVERIFY(store.findById(source.id));
    QCOMPARE(store.tagCooccurrenceGraph().weightBetween(QStringLiteral("alpha"), QStringLiteral("beta")), 0);
    QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM memory_index_jobs")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 2);
    query.finish();
    QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM sleep_staged_change WHERE status='Finalized'")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 0);
    query.finish();
    QVERIFY(query.exec(QStringLiteral("DROP TRIGGER fail_second_long_term")));
    const auto retried = consolidator.applyDecisions(snapshot, decisions);
    QVERIFY(retried.committed);
    QCOMPARE(retried.upgraded, 2);
    QCOMPARE(store.all().size(), 2);
    for (const auto& entry : store.all()) QCOMPARE(entry.type, MemoryType::Episodic);
    QCOMPARE(store.tagCooccurrenceGraph().weightBetween(QStringLiteral("alpha"), QStringLiteral("beta")), 2);
}

void TestMemoryStrategy::testDaydreamUpdatesTagCooccurrenceGraph() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);
    MemoryEntry source;
    source.type = MemoryType::ShortTerm;
    source.key = QStringLiteral("tag_graph");
    source.summary = QStringLiteral("用户使用 Qt 和 C++");
    source.content = source.summary;
    source.source = QStringLiteral("user_interaction");
    source.tags = {
        QStringLiteral("Qt"),
        QStringLiteral(" C++ "),
        QStringLiteral("daydream_inbox")
    };
    source.mentionCount = 2;
    QVERIFY(!store.addEntry(source).id.isEmpty());

    DaydreamConsolidator consolidator(store);
    const DaydreamConsolidator::Stats stats = consolidator.runHardcodedDrain();
    QVERIFY(stats.committed);
    QCOMPARE(stats.upgraded, 1);
    QCOMPARE(store.tagCooccurrenceGraph().weightBetween(
        QStringLiteral(" qt "), QStringLiteral("C++")), 1);
    QCOMPARE(store.tagCooccurrenceGraph().weightBetween(
        QStringLiteral("daydream_inbox"), QStringLiteral("qt")), 0);
    QVERIFY(!store.all().first().tags.contains(
        QStringLiteral("daydream_inbox"), Qt::CaseInsensitive));
}

void TestMemoryStrategy::testDaydreamUpdateRecordsTagCooccurrence() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);
    const MemoryEntry target = store.add(
        MemoryType::Semantic,
        QStringLiteral("tooling"),
        QStringLiteral("用户使用 Qt"),
        {QStringLiteral("Qt")});
    QVERIFY(!target.id.isEmpty());

    MemoryEntry source;
    source.type = MemoryType::ShortTerm;
    source.key = QStringLiteral("tooling_update");
    source.summary = QStringLiteral("用户也使用 C++");
    source.content = source.summary;
    source.source = QStringLiteral("user_interaction");
    source.tags = {QStringLiteral("C++")};
    const MemoryEntry storedSource = store.addEntry(source);
    QVERIFY(!storedSource.id.isEmpty());

    DaydreamConsolidator consolidator(store);
    const DaydreamConsolidator::Snapshot snapshot = consolidator.createSnapshot();
    QCOMPARE(snapshot.size(), 1);
    DaydreamConsolidator::Decision decision;
    decision.sourceId = storedSource.id;
    decision.action = DaydreamConsolidator::Action::Update;
    decision.targetType = MemoryType::Semantic;
    decision.targetMemoryId = target.id;
    decision.expectedTarget = target;
    decision.mergedContent = QStringLiteral("用户使用 Qt 和 C++");
    decision.qualityScore = 8.0;
    decision.tags = {QStringLiteral("Qt"), QStringLiteral("C++")};

    const DaydreamConsolidator::Stats stats = consolidator.applyDecisions(
        snapshot, {decision});
    QVERIFY(stats.committed);
    QCOMPARE(stats.updated, 1);
    QCOMPARE(store.tagCooccurrenceGraph().weightBetween(
        QStringLiteral("qt"), QStringLiteral("c++")), 1);
    QVERIFY(!store.findById(storedSource.id));
}

void TestMemoryStrategy::testDaydreamTagCooccurrenceAccumulates() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);
    for (int i = 0; i < 2; ++i) {
        MemoryEntry source;
        source.type = MemoryType::ShortTerm;
        source.key = QStringLiteral("tag_graph_%1").arg(i);
        source.summary = QStringLiteral("标签共现 %1").arg(i);
        source.content = source.summary;
        source.source = QStringLiteral("user_interaction");
        source.tags = i == 0
            ? QStringList{QStringLiteral("Qt"), QStringLiteral("C++")}
            : QStringList{QStringLiteral(" qt "), QStringLiteral("c++")};
        source.mentionCount = 2;
        QVERIFY(!store.addEntry(source).id.isEmpty());

        DaydreamConsolidator consolidator(store);
        QVERIFY(consolidator.runHardcodedDrain().committed);
    }

    QCOMPARE(store.tagCooccurrenceGraph().weightBetween(
        QStringLiteral("QT"), QStringLiteral("c++")), 2);
    const QList<TagCooccurrence> neighbors = store.tagCooccurrenceGraph().neighborsOf(
        QStringLiteral("qt"));
    QCOMPARE(neighbors.size(), 1);
    QCOMPARE(neighbors.first().weight, 2);
}

void TestMemoryStrategy::testDaydreamDrainDiscardsLowValue() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry chitchat;
    chitchat.type = MemoryType::ShortTerm;
    chitchat.key = QStringLiteral("chitchat");
    chitchat.summary = QStringLiteral("一次普通闲聊");
    chitchat.content = chitchat.summary;
    chitchat.source = QStringLiteral("assistant_response");
    chitchat.importance = 0.2;
    chitchat.mentionCount = 1; // 不满足 >=2，emotion 为 0 → discard
    const QString id = store.addEntry(chitchat).id;
    QVERIFY(store.load());

    DaydreamConsolidator consolidator(store);
    const DaydreamConsolidator::Stats stats = consolidator.runHardcodedDrain();
    QVERIFY(stats.committed);
    QCOMPARE(stats.scanned, 1);
    QCOMPARE(stats.upgraded, 0);
    QCOMPARE(stats.discarded, 1);

    QVERIFY(store.load());
    QCOMPARE(store.all().size(), 0);
    QVERIFY(!store.findById(id));
}

void TestMemoryStrategy::testDaydreamDrainSparesOtherPartitions() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    // 一条 Semantic 长期记忆（不在 Hippocampus），不应被 drain 触碰。
    store.add(MemoryType::Semantic, QStringLiteral("fact"), QStringLiteral("用户用 Qt6"), {QStringLiteral("tech")});
    // 一条 Hippocampus 低价值条目，会被 discard。
    MemoryEntry junk;
    junk.type = MemoryType::ShortTerm;
    junk.key = QStringLiteral("junk");
    junk.summary = QStringLiteral("噪音");
    junk.content = junk.summary;
    junk.importance = 0.1;
    store.addEntry(junk);
    QVERIFY(store.load());
    const int totalBefore = store.all().size();

    DaydreamConsolidator consolidator(store);
    const DaydreamConsolidator::Stats stats = consolidator.runHardcodedDrain();
    QVERIFY(stats.committed);
    QCOMPARE(stats.scanned, 1); // 只扫到 1 条 Hippocampus
    QCOMPARE(stats.discarded, 1);

    QVERIFY(store.load());
    // Semantic 那条仍在；Hippocampus 那条被删 → 总数减 1。
    QCOMPARE(store.all().size(), totalBefore - 1);
    bool semanticKept = false;
    for (const MemoryEntry& e : store.all()) {
        if (e.type == MemoryType::Semantic && e.key == QStringLiteral("fact")) semanticKept = true;
    }
    QVERIFY(semanticKept);
}

// 回归：memory_items.key 此前 loadAll 漏读，读回恒为空。验证 key 往返持久化。
void TestMemoryStrategy::testStoreKeyPersistsRoundtrip() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);
    store.add(MemoryType::Semantic, QStringLiteral("fact"), QStringLiteral("用户用 Qt6"), {QStringLiteral("tech")});
    QVERIFY(store.load());

    QCOMPARE(store.all().size(), 1);
    QCOMPARE(store.all().first().key, QStringLiteral("fact"));
}

// Exact repeated user impressions are coalesced by the production path. Verify
// that the persisted recurrence signal can drive the offline fallback.
void TestMemoryStrategy::testDaydreamDrainUpgradesViaPersistedMentionCount() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryExtractor extractor;
    MemoryEntry impression = extractor.extractDaydreamImpression(
        QStringLiteral("我反复在准备面试安排"), QStringLiteral("user_request"));
    const MemoryEntry stored = store.addEntry(impression);
    QVERIFY(!stored.id.isEmpty());
    impression = stored;
    impression.mentionCount = 2;
    impression.updatedAt = impression.updatedAt.addMSecs(1);
    QVERIFY(store.updateEntryById(impression));
    QVERIFY(store.load());

    QCOMPARE(store.all().size(), 1);
    QCOMPARE(store.all().first().mentionCount, 2); // recurrence 信号已持久化
    QCOMPARE(store.all().first().partition, QStringLiteral("hippocampus"));

    DaydreamConsolidator consolidator(store);
    const DaydreamConsolidator::Stats stats = consolidator.runHardcodedDrain();
    QVERIFY(stats.committed);
    QCOMPARE(stats.scanned, 1);
    QCOMPARE(stats.upgraded, 1); // mentionCount>=2 → 升级而非 discard
    QCOMPARE(stats.discarded, 0);

    QVERIFY(store.load());
    QCOMPARE(store.all().size(), 1);
    const MemoryEntry upgraded = store.all().first();
    QCOMPARE(upgraded.type, MemoryType::Episodic);
    QCOMPARE(upgraded.privacyLevel, PrivacyLevel::Personal); // review finding #3
    QCOMPARE(upgraded.source, QStringLiteral("daydream"));
}

void TestMemoryStrategy::testDaydreamFallbackUpgradesHighImportance() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryExtractor extractor;
    MemoryEntry impression = extractor.extractDaydreamImpression(
        QStringLiteral("我决定换一份新工作了"), QStringLiteral("user_request"));
    QVERIFY(!impression.key.isEmpty()); // 自我披露检查通过
    impression.importance = 0.7; // 高重要性（≥ 0.6 门槛），mentionCount 仍为 1
    const MemoryEntry stored = store.addEntry(impression);
    QVERIFY(!stored.id.isEmpty());
    QCOMPARE(stored.partition, QStringLiteral("hippocampus"));

    DaydreamConsolidator consolidator(store);
    const DaydreamConsolidator::Stats stats = consolidator.runHardcodedDrain();
    QVERIFY(stats.committed);
    QCOMPARE(stats.scanned, 1);
    QCOMPARE(stats.upgraded, 1); // importance >= 0.6 → 升级（新增兜底规则）
    QCOMPARE(stats.discarded, 0);

    QVERIFY(store.load());
    QCOMPARE(store.all().size(), 1);
    const MemoryEntry upgraded = store.all().first();
    QVERIFY(upgraded.partition != QLatin1String("hippocampus"));
    QCOMPARE(upgraded.type, MemoryType::Episodic); // 无关键词命中 → 默认 Episodic
}

void TestMemoryStrategy::testDaydreamFallbackRoutesPreferenceKeyword() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryExtractor extractor;
    MemoryEntry impression = extractor.extractDaydreamImpression(
        QStringLiteral("我喜欢在深夜写代码"), QStringLiteral("user_request"));
    QVERIFY(!impression.key.isEmpty());
    impression.importance = 0.7; // 达到升级门槛
    const MemoryEntry stored = store.addEntry(impression);
    QVERIFY(!stored.id.isEmpty());

    DaydreamConsolidator consolidator(store);
    const DaydreamConsolidator::Stats stats = consolidator.runHardcodedDrain();
    QVERIFY(stats.committed);
    QCOMPARE(stats.scanned, 1);
    QCOMPARE(stats.upgraded, 1);
    QCOMPARE(stats.discarded, 0);

    QVERIFY(store.load());
    QCOMPARE(store.all().size(), 1);
    const MemoryEntry upgraded = store.all().first();
    QCOMPARE(upgraded.type, MemoryType::Preference); // 「喜欢」→ 路由为偏好
    QCOMPARE(upgraded.partition, QStringLiteral("preference"));
}

void TestMemoryStrategy::testDaydreamFallbackDeduplicatesBatch() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryExtractor extractor;
    // 3 条内容相同的高重要性印象（模拟重复采集）
    for (int i = 0; i < 3; ++i) {
        MemoryEntry impression = extractor.extractDaydreamImpression(
            QStringLiteral("我最近在反复准备面试"), QStringLiteral("user_request"));
        QVERIFY(!impression.key.isEmpty());
        impression.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        impression.key = QStringLiteral("daydream:test:%1").arg(i); // 避免 key 覆盖
        impression.importance = 0.7;
        const MemoryEntry stored = store.addEntry(impression);
        QVERIFY(!stored.id.isEmpty());
    }
    QCOMPARE(store.all().size(), 3);

    DaydreamConsolidator consolidator(store);
    const DaydreamConsolidator::Stats stats = consolidator.runHardcodedDrain();
    QVERIFY(stats.committed);
    QCOMPARE(stats.scanned, 3);
    QCOMPARE(stats.upgraded, 1); // 批内去重：相同正文只升级第一条
    QCOMPARE(stats.discarded, 2);

    QVERIFY(store.load());
    QCOMPARE(store.all().size(), 1);
}

void TestMemoryStrategy::testDaydreamDiscardsLegacyAssistantInbox() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry legacy;
    legacy.type = MemoryType::ShortTerm;
    legacy.key = QStringLiteral("assistant_response");
    legacy.summary = QStringLiteral("桌宠自己说过的话");
    legacy.content = legacy.summary;
    legacy.source = QStringLiteral("assistant_inferred");
    legacy.tags = {QStringLiteral("assistant")};
    legacy.mentionCount = 5;
    QVERIFY(!store.addEntry(legacy).id.isEmpty());

    DaydreamConsolidator consolidator(store);
    QVERIFY(!DaydreamConsolidator::requiresModelDecision(store.all().first()));
    const DaydreamConsolidator::Stats stats = consolidator.runHardcodedDrain();
    QVERIFY(stats.committed);
    QCOMPARE(stats.upgraded, 0);
    QCOMPARE(stats.discarded, 1);
    QCOMPARE(store.all().size(), 0);
}

void TestMemoryStrategy::testDaydreamSessionLimitLeavesRemainder() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    for (int i = 0; i < DaydreamConsolidator::SESSION_LIMIT + 3; ++i) {
        MemoryEntry item;
        item.type = MemoryType::ShortTerm;
        item.key = QStringLiteral("pending_%1").arg(i);
        item.summary = QStringLiteral("低价值片段 %1").arg(i);
        item.content = item.summary;
        item.source = QStringLiteral("user_interaction");
        item.mentionCount = 1;
        QVERIFY(!store.addEntry(item).id.isEmpty());
    }

    DaydreamConsolidator consolidator(store);
    const DaydreamConsolidator::Stats stats = consolidator.runHardcodedDrain();
    QVERIFY(stats.committed);
    // Phase 4: 批次选择器将整批上限设为 20（设计："整批最多 20 条"），
    // 而非旧的 SESSION_LIMIT=32。35 条候选中处理 20 条，剩余 15 条。
    QCOMPARE(stats.scanned, 20);
    QCOMPARE(stats.discarded, 20);
    QCOMPARE(consolidator.pendingCount(), DaydreamConsolidator::SESSION_LIMIT + 3 - 20);
}

void TestMemoryStrategy::testDaydreamRejectsStaleSnapshotAtomically() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    for (int i = 0; i < 2; ++i) {
        MemoryEntry item;
        item.type = MemoryType::ShortTerm;
        item.key = QStringLiteral("stale_%1").arg(i);
        item.summary = item.key;
        item.content = item.key;
        item.source = QStringLiteral("user_interaction");
        QVERIFY(!store.addEntry(item).id.isEmpty());
    }

    DaydreamConsolidator consolidator(store);
    const DaydreamConsolidator::Snapshot snapshot = consolidator.createSnapshot();
    QCOMPARE(snapshot.size(), 2);
    const QList<DaydreamConsolidator::Decision> decisions =
        DaydreamConsolidator::hardcodedDecisions(snapshot.items);

    MemoryEntry changed = *store.findById(snapshot.items.first().id);
    changed.content += QStringLiteral(" changed");
    changed.updatedAt = changed.updatedAt.addMSecs(1);
    QVERIFY(store.updateEntryById(changed));

    const DaydreamConsolidator::Stats stats = consolidator.applyDecisions(snapshot, decisions);
    QVERIFY(!stats.committed);
    QVERIFY(stats.staleSnapshot);
    QCOMPARE(consolidator.pendingCount(), 2);
}

void TestMemoryStrategy::testDaydreamRejectsStaleUpdateTarget() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry source;
    source.type = MemoryType::ShortTerm;
    source.key = QStringLiteral("source");
    source.summary = QStringLiteral("我更喜欢紧凑界面");
    source.content = source.summary;
    source.source = QStringLiteral("user_interaction");
    QVERIFY(!store.addEntry(source).id.isEmpty());

    MemoryEntry target;
    target.type = MemoryType::Preference;
    target.key = QStringLiteral("ui_preference");
    target.summary = QStringLiteral("用户喜欢宽松界面");
    target.content = target.summary;
    target.source = QStringLiteral("user_explicit");
    const MemoryEntry storedTarget = store.addEntry(target);
    QVERIFY(!storedTarget.id.isEmpty());

    DaydreamConsolidator consolidator(store);
    const DaydreamConsolidator::Snapshot snapshot = consolidator.createSnapshot();
    const QString response = QStringLiteral(
        "[{\"source_id\":\"%1\",\"target_partition\":\"Preference\","
        "\"action\":\"update\",\"target_memory_id\":\"%2\","
        "\"merged_content\":\"用户现在更喜欢紧凑界面\",\"quality_score\":8}]")
        .arg(snapshot.items.first().id, storedTarget.id);
    QList<DaydreamConsolidator::Decision> decisions;
    QString error;
    QVERIFY2(DaydreamConsolidator::parseDecisions(
        response, snapshot.items, {storedTarget}, &decisions, &error), qPrintable(error));

    MemoryEntry changedTarget = *store.findById(storedTarget.id);
    changedTarget.content = QStringLiteral("用户刚刚明确要求保持宽松界面");
    changedTarget.summary = changedTarget.content;
    changedTarget.updatedAt = changedTarget.updatedAt.addMSecs(1);
    QVERIFY(store.updateEntryById(changedTarget));

    const DaydreamConsolidator::Stats stats = consolidator.applyDecisions(snapshot, decisions);
    QVERIFY(!stats.committed);
    QVERIFY(stats.staleSnapshot);
    QVERIFY(store.findById(snapshot.items.first().id));
    QCOMPARE(store.findById(storedTarget.id)->content, changedTarget.content);
}

void TestMemoryStrategy::testDaydreamSnapshotDoesNotConsumeNewInboxItems() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry original;
    original.type = MemoryType::ShortTerm;
    original.key = QStringLiteral("original");
    original.summary = original.key;
    original.content = original.key;
    original.source = QStringLiteral("user_interaction");
    QVERIFY(!store.addEntry(original).id.isEmpty());

    DaydreamConsolidator consolidator(store);
    const DaydreamConsolidator::Snapshot snapshot = consolidator.createSnapshot();
    QCOMPARE(snapshot.size(), 1);

    MemoryEntry arrivedLater = original;
    arrivedLater.id.clear();
    arrivedLater.key = QStringLiteral("arrived_later");
    arrivedLater.summary = arrivedLater.key;
    arrivedLater.content = arrivedLater.key;
    const QString newId = store.addEntry(arrivedLater).id;
    QVERIFY(!newId.isEmpty());

    const DaydreamConsolidator::Stats stats = consolidator.applyDecisions(
        snapshot, DaydreamConsolidator::hardcodedDecisions(snapshot.items));
    QVERIFY(stats.committed);
    QCOMPARE(consolidator.pendingCount(), 1);
    QVERIFY(store.findById(newId));
}

void TestMemoryStrategy::testDaydreamParsesValidatedLlmDecisions() {
    MemoryEntry preference;
    preference.id = QStringLiteral("source-pref");
    MemoryEntry noise;
    noise.id = QStringLiteral("source-noise");
    const QList<MemoryEntry> batch = {preference, noise};

    const QString response = QStringLiteral(R"JSON(
```json
[
  {"source_id":"source-pref","target_partition":"Preference","action":"create",
   "target_memory_id":"","merged_content":"用户偏好紧凑界面","quality_score":8,
   "new_tags":["ui","preference"]},
  {"source_id":"source-noise","action":"discard","quality_score":1,"new_tags":[]}
]
```
)JSON");
    QList<DaydreamConsolidator::Decision> decisions;
    QString error;
    QVERIFY2(DaydreamConsolidator::parseDecisions(response, batch, {}, &decisions, &error),
             qPrintable(error));
    QCOMPARE(decisions.size(), 2);
    QCOMPARE(decisions.first().targetType, MemoryType::Preference);
    QCOMPARE(decisions.first().action, DaydreamConsolidator::Action::Create);
    QCOMPARE(decisions.last().action, DaydreamConsolidator::Action::Discard);

    const QString duplicate = QStringLiteral(
        "[{\"source_id\":\"source-pref\",\"action\":\"discard\"},"
        "{\"source_id\":\"source-pref\",\"action\":\"discard\"}]");
    QVERIFY(!DaydreamConsolidator::parseDecisions(duplicate, batch, {}, &decisions, &error));

    const QString unauthorizedUpdate = QStringLiteral(
        "[{\"source_id\":\"source-pref\",\"target_partition\":\"Preference\","
        "\"action\":\"update\",\"target_memory_id\":\"not-shown\"},"
        "{\"source_id\":\"source-noise\",\"action\":\"discard\"}]");
    QVERIFY(!DaydreamConsolidator::parseDecisions(
        unauthorizedUpdate, batch, {}, &decisions, &error));
}

// Phase 4.2.5：对象根 {decisions, relations} 解析 + 提案校验（设计 §10）。
void TestMemoryStrategy::testDaydreamParsesRelationsProposalsFromObjectRoot() {
    MemoryEntry a;
    a.id = QStringLiteral("source-a");
    MemoryEntry b;
    b.id = QStringLiteral("source-b");
    const QList<MemoryEntry> batch = {a, b};
    const QList<QPair<QString, QString>> candidates = {
        {QStringLiteral("source-a"), QStringLiteral("source-b")}};

    // 合法对象根：决策数组 + 1 条合法提案
    const QString response = QStringLiteral(R"JSON(
{
  "decisions": [
    {"source_id":"source-a","target_partition":"Semantic","action":"create",
     "target_memory_id":"","merged_content":"a","quality_score":7,"new_tags":[]},
    {"source_id":"source-b","action":"discard","quality_score":1,"new_tags":[]}
  ],
  "relations": [
    {"from_source_id":"source-a","to_source_id":"source-b",
     "relation":"topic_of","confidence":0.85,"evidence":"同一主题"}
  ]
}
)JSON");
    QList<DaydreamConsolidator::Decision> decisions;
    QList<RelationProposal> proposals;
    QString error;
    QVERIFY2(DaydreamConsolidator::parseDecisions(
                 response, batch, {}, &decisions, &error, &proposals, candidates),
             qPrintable(error));
    QCOMPARE(decisions.size(), 2);
    QCOMPARE(proposals.size(), 1);
    QCOMPARE(proposals.first().type, MemoryRelationType::TopicOf);
    QCOMPARE(proposals.first().confidence, 0.85);

    // 非候选对 → 丢弃（设计：只对候选集合内的关系做判断）
    const QList<QPair<QString, QString>> otherCandidates = {};
    QList<RelationProposal> filtered;
    QVERIFY(DaydreamConsolidator::parseDecisions(
        response, batch, {}, &decisions, &error, &filtered, otherCandidates));
    // candidates 为空时不做候选限制（宽松）；提供非匹配候选时丢弃
    const QList<QPair<QString, QString>> mismatched = {
        {QStringLiteral("source-b"), QStringLiteral("source-a")}};
    // 反序命中（候选对无向语义）
    QList<RelationProposal> reverse;
    QVERIFY(DaydreamConsolidator::parseDecisions(
        response, batch, {}, &decisions, &error, &reverse, mismatched));
    QCOMPARE(reverse.size(), 1);

    // 引用批次外 id 的提案 → 静默丢弃，决策仍有效
    const QString ghostProposal = QStringLiteral(R"JSON(
{
  "decisions": [
    {"source_id":"source-a","target_partition":"Semantic","action":"create",
     "target_memory_id":"","merged_content":"a","quality_score":7,"new_tags":[]},
    {"source_id":"source-b","action":"discard","quality_score":1,"new_tags":[]}
  ],
  "relations": [
    {"from_source_id":"source-a","to_source_id":"ghost",
     "relation":"topic_of","confidence":0.9,"evidence":"e"}
  ]
}
)JSON");
    QList<RelationProposal> dropped;
    QVERIFY(DaydreamConsolidator::parseDecisions(
        ghostProposal, batch, {}, &decisions, &error, &dropped, candidates));
    QCOMPARE(decisions.size(), 2);
    QCOMPARE(dropped.size(), 0);

    // 非法类型（related 非模型可判断）与缺证据 → 丢弃
    const QString invalidProposals = QStringLiteral(R"JSON(
{
  "decisions": [
    {"source_id":"source-a","target_partition":"Semantic","action":"create",
     "target_memory_id":"","merged_content":"a","quality_score":7,"new_tags":[]},
    {"source_id":"source-b","action":"discard","quality_score":1,"new_tags":[]}
  ],
  "relations": [
    {"from_source_id":"source-a","to_source_id":"source-b",
     "relation":"related","confidence":0.9,"evidence":"e"},
    {"from_source_id":"source-a","to_source_id":"source-b",
     "relation":"topic_of","confidence":0.9,"evidence":""}
  ]
}
)JSON");
    QList<RelationProposal> invalid;
    QVERIFY(DaydreamConsolidator::parseDecisions(
        invalidProposals, batch, {}, &decisions, &error, &invalid, candidates));
    QCOMPARE(invalid.size(), 0);

    // 旧格式纯数组仍兼容，proposals 为空
    const QString legacyArray = QStringLiteral(
        "[{\"source_id\":\"source-a\",\"target_partition\":\"Semantic\",\"action\":\"create\","
        "\"target_memory_id\":\"\",\"merged_content\":\"a\",\"quality_score\":7,\"new_tags\":[]},"
        "{\"source_id\":\"source-b\",\"action\":\"discard\",\"quality_score\":1,\"new_tags\":[]}]");
    QList<RelationProposal> none;
    QVERIFY(DaydreamConsolidator::parseDecisions(
        legacyArray, batch, {}, &decisions, &error, &none, candidates));
    QCOMPARE(decisions.size(), 2);
    QCOMPARE(none.size(), 0);
}

// Phase 4.2.5：buildChangeSet 校验提案（引用批次内节点、上限 8、去重）+ 携带进哈希。
void TestMemoryStrategy::testDaydreamBuildChangeSetValidatesProposals() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    // 两条 Active hippocampus 候选
    MemoryEntry a;
    a.type = MemoryType::ShortTerm;
    a.key = QStringLiteral("k-a");
    a.summary = QStringLiteral("a");
    a.content = a.summary;
    a.mentionCount = 3;
    const QString idA = store.addEntry(a).id;
    MemoryEntry b;
    b.type = MemoryType::ShortTerm;
    b.key = QStringLiteral("k-b");
    b.summary = QStringLiteral("b");
    b.content = b.summary;
    b.mentionCount = 3;
    const QString idB = store.addEntry(b).id;
    QVERIFY(store.load());

    DaydreamConsolidator consolidator(store);
    DaydreamConsolidator::Snapshot snapshot;
    for (const MemoryEntry& entry : store.all()) {
        if (entry.partition == QLatin1String("hippocampus")) {
            snapshot.items.append(entry);
        }
    }
    QCOMPARE(snapshot.items.size(), 2);

    QList<DaydreamConsolidator::Decision> decisions;
    for (const MemoryEntry& entry : snapshot.items) {
        DaydreamConsolidator::Decision decision;
        decision.sourceId = entry.id;
        decision.action = DaydreamConsolidator::Action::Create;
        decision.targetType = MemoryType::Episodic;
        decisions.append(decision);
    }

    // 合法提案
    RelationProposal good;
    good.fromMemoryId = idA;
    good.toMemoryId = idB;
    good.type = MemoryRelationType::TopicOf;
    good.confidence = 0.8;
    good.evidence = QStringLiteral("同一主题");

    // 非法：引用批次外节点
    RelationProposal ghost = good;
    ghost.toMemoryId = QStringLiteral("ghost");

    const auto changeSet = consolidator.buildChangeSet(
        snapshot, decisions, {good, ghost, good});  // 合法 + 非法 + 重复
    QVERIFY(changeSet.isOk());
    QCOMPARE(changeSet.value().relationProposals.size(), 1);  // 只保留合法且去重后的 1 条
    QCOMPARE(changeSet.value().relationProposals.first().fromMemoryId, idA);
}

// Phase 4.2.5：哈希兼容性——旧载荷（无提案）与新载荷（有提案）各自可验证。
void TestMemoryStrategy::testDaydreamChangeSetProposalsHashCompatWithLegacyPayload() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    MemoryEntry a;
    a.type = MemoryType::ShortTerm;
    a.key = QStringLiteral("k-a");
    a.summary = QStringLiteral("a");
    a.content = a.summary;
    a.mentionCount = 3;
    const QString idA = store.addEntry(a).id;
    MemoryEntry b;
    b.type = MemoryType::ShortTerm;
    b.key = QStringLiteral("k-b");
    b.summary = QStringLiteral("b");
    b.content = b.summary;
    b.mentionCount = 3;
    const QString idB = store.addEntry(b).id;
    QVERIFY(store.load());

    DaydreamConsolidator consolidator(store);
    DaydreamConsolidator::Snapshot snapshot;
    for (const MemoryEntry& entry : store.all()) {
        if (entry.partition == QLatin1String("hippocampus")) {
            snapshot.items.append(entry);
        }
    }
    QCOMPARE(snapshot.items.size(), 2);
    QList<DaydreamConsolidator::Decision> decisions;
    for (const MemoryEntry& entry : snapshot.items) {
        DaydreamConsolidator::Decision decision;
        decision.sourceId = entry.id;
        decision.action = DaydreamConsolidator::Action::Create;
        decision.targetType = MemoryType::Episodic;
        decisions.append(decision);
    }

    // 旧载荷：无提案 → toJson 不含 relationProposals 键，fromJson 往返哈希不变。
    const auto legacyBuilt = consolidator.buildChangeSet(snapshot, decisions, {});
    QVERIFY(legacyBuilt.isOk());
    const QJsonObject legacyJson = legacyBuilt.value().toJson();
    QVERIFY(!legacyJson.contains(QStringLiteral("relationProposals")));
    const auto parsedLegacy = DaydreamChangeSet::fromJson(legacyJson);
    QVERIFY(parsedLegacy.isOk());
    QCOMPARE(parsedLegacy.value().relationProposals.size(), 0);
    QCOMPARE(parsedLegacy.value().changeSetId, legacyBuilt.value().changeSetId);

    // 新载荷：有提案 → 哈希覆盖提案，fromJson 往返一致。
    RelationProposal proposal;
    proposal.fromMemoryId = idA;
    proposal.toMemoryId = idB;
    proposal.type = MemoryRelationType::TopicOf;
    proposal.confidence = 0.8;
    proposal.evidence = QStringLiteral("同一主题");
    const auto proposalsBuilt = consolidator.buildChangeSet(
        snapshot, decisions, {proposal});
    QVERIFY(proposalsBuilt.isOk());
    QCOMPARE(proposalsBuilt.value().relationProposals.size(), 1);
    const QJsonObject proposalsJson = proposalsBuilt.value().toJson();
    QVERIFY(proposalsJson.contains(QStringLiteral("relationProposals")));
    const auto parsedProposals = DaydreamChangeSet::fromJson(proposalsJson);
    QVERIFY(parsedProposals.isOk());
    QCOMPARE(parsedProposals.value().relationProposals.size(), 1);
    QCOMPARE(parsedProposals.value().relationProposals.first().fromMemoryId, idA);
    QCOMPARE(parsedProposals.value().relationProposals.first().evidence,
             QStringLiteral("同一主题"));
    QCOMPARE(parsedProposals.value().changeSetId, proposalsBuilt.value().changeSetId);

    // 提案进入哈希：篡改提案 → 哈希失配被拒。
    QJsonObject tampered = proposalsJson;
    QJsonArray tamperedProposals = tampered.value(QStringLiteral("relationProposals")).toArray();
    QJsonObject p0 = tamperedProposals.first().toObject();
    p0.insert(QStringLiteral("evidence"), QStringLiteral("被篡改的证据"));
    tamperedProposals[0] = p0;
    tampered.insert(QStringLiteral("relationProposals"), tamperedProposals);
    const auto parsedTampered = DaydreamChangeSet::fromJson(tampered);
    QVERIFY(!parsedTampered.isOk());

    // 新旧哈希不同
    QVERIFY(proposalsBuilt.value().changeSetId != legacyBuilt.value().changeSetId);
}

// DaydreamTriggerPolicy 复合判定：全条件满足才触发。
void TestMemoryStrategy::testDaydreamTriggerPolicyAllConditions() {
    DaydreamTriggerPolicy policy;
    // idle=600>=N1(300) && !busy && msToNext=1200000>=N2(600000) &&
    // msSinceLast=1000000>=MIN_GAP(900000) && !interrupted && count=0<HOURLY_CAP(3)
    QVERIFY(policy.shouldTrigger(600, false, 1200000, 1000000, false, 0));
    // 刚好边界：idle=N1, msToNext=N2, msSinceLast=MIN_GAP
    QVERIFY(policy.shouldTrigger(300, false, 600000, 900000, false, 0));
}

void TestMemoryStrategy::testDaydreamTriggerPolicyNegativeCases() {
    DaydreamTriggerPolicy policy;
    const bool wasI = false;
    // 任一条件不满足 → false
    QVERIFY(!policy.shouldTrigger(299, false, 1200000, 1000000, wasI, 0)); // idle 不足
    QVERIFY(!policy.shouldTrigger(600, true, 1200000, 1000000, wasI, 0));  // busy
    QVERIFY(!policy.shouldTrigger(600, false, 599999, 1000000, wasI, 0));  // 待办近
    QVERIFY(!policy.shouldTrigger(600, false, 1200000, 899999, wasI, 0));  // 距上次不足
    QVERIFY(!policy.shouldTrigger(600, false, 1200000, 1000000, wasI, 3)); // 超每小时上限
    QVERIFY(!policy.shouldTrigger(-1, false, 1200000, 1000000, wasI, 0));  // 平台不支持
    //被打断需叠加 BACKOFF：MIN_GAP+BACKOFF=1500000，msSinceLast=1000000 不足
    QVERIFY(!policy.shouldTrigger(600, false, 1200000, 1000000, true, 0));
    QVERIFY(policy.shouldTrigger(600, false, 1200000, 1500000, true, 0));  // 退避过后
}

// 无待办(msToNextDue<0)不阻塞触发。
void TestMemoryStrategy::testDaydreamTriggerPolicyNoDueTodoNonBlocking() {
    DaydreamTriggerPolicy policy;
    QVERIFY(policy.shouldTrigger(600, false, -1, 1000000, false, 0));
}

void TestMemoryStrategy::testDaydreamTriggerPolicyContinuation() {
    DaydreamTriggerPolicy policy;
    QVERIFY(policy.shouldContinue(300, false, 600000));
    QVERIFY(policy.shouldContinue(600, false, -1));
    QVERIFY(!policy.shouldContinue(299, false, 600000));
    QVERIFY(!policy.shouldContinue(600, true, 600000));
    QVERIFY(!policy.shouldContinue(600, false, 599999));
}

void TestMemoryStrategy::testDaydreamTriggerPolicyUsesRuntimeConfig() {
    DaydreamConfig config;
    config.idleThresholdSec = 60;
    config.dueSoonThresholdMs = 120000;
    config.minIntervalMs = 180000;
    config.interruptionBackoffMs = 60000;
    config.hourlyLimit = 1;
    config.tickIntervalMs = 5000;
    DaydreamTriggerPolicy policy(config);

    QVERIFY(policy.shouldTrigger(60, false, 120000, 180000, false, 0));
    QVERIFY(!policy.shouldTrigger(59, false, 120000, 180000, false, 0));
    QVERIFY(!policy.shouldTrigger(60, false, 119999, 180000, false, 0));
    QVERIFY(!policy.shouldTrigger(60, false, 120000, 179999, false, 0));
    QVERIFY(!policy.shouldTrigger(60, false, 120000, 180000, false, 1));
    QVERIFY(policy.shouldTrigger(60, false, 120000, 240000, true, 0));
    QCOMPARE(policy.requiredGapMs(true), qint64(240000));
    QCOMPARE(policy.nextTickMs(-1), 5000);
}

#ifdef DESKTOP_PET_HAS_ORT
// OnnxEmbeddingProvider：加载 assets/embeddings/model_quantized.onnx + vocab.txt，
// 验证维度 512、归一化(norm≈1)、语义近邻(相似文本余弦 > 不相关文本)。
// 模型未生成时 QSKIP，不阻塞回归。
void TestMemoryStrategy::testOnnxEmbeddingProviderLoadsAndEmbeds() {
    const QString dir = QStringLiteral(DESKTOP_PET_EMBEDDING_ASSETS);
    const QString model = QDir(dir).filePath(QStringLiteral("model_quantized.onnx"));
    const QString vocab = QDir(dir).filePath(QStringLiteral("vocab.txt"));
    if (!QFile::exists(model) || !QFile::exists(vocab)) {
        QSKIP("onnx embedding assets not present; run tools/export_bge_onnx.py");
    }

    OnnxEmbeddingProvider provider;
    QString err;
    OnnxEmbeddingProvider::Config cfg;
    cfg.modelPath = model;
    cfg.vocabPath = vocab;
    cfg.maxSeqLen = 64;
    QVERIFY(provider.load(cfg, &err));
    QCOMPARE(provider.dimension(), 512);

    const QVector<float> a = provider.embed(QStringLiteral("用户喜欢c++编程语言"));
    QCOMPARE(a.size(), 512);
    double norm = 0.0;
    for (float v : a) norm += double(v) * double(v);
    norm = std::sqrt(norm);
    QVERIFY(norm > 0.95 && norm < 1.05); // L2 归一化后范数≈1

    const QVector<float> b = provider.embed(QStringLiteral("用户偏爱C++程序设计"));
    const QVector<float> c = provider.embed(QStringLiteral("今天天气晴朗适合出门散步"));

    auto cosine = [](const QVector<float>& x, const QVector<float>& y) {
        double dot = 0.0, na = 0.0, nb = 0.0;
        for (int i = 0; i < x.size(); ++i) { dot += double(x[i]) * double(y[i]); na += double(x[i]) * double(x[i]); nb += double(y[i]) * double(y[i]); }
        if (na <= 0.0 || nb <= 0.0) return -1.0;
        return dot / (std::sqrt(na) * std::sqrt(nb));
    };
    const double simSimilar = cosine(a, b);
    const double simUnrelated = cosine(a, c);
    QVERIFY(simSimilar > 0.5);
    QVERIFY(simSimilar > simUnrelated);
}
#endif

QTEST_MAIN(TestMemoryStrategy)
#include "test_memory_strategy.moc"
