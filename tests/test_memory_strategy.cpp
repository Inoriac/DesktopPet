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
    void testPolicyWritesAndSkipsDuplicate();
    void testPolicyRejectsSensitiveMemory();
    void testPolicyMarksMatchedMemoryDeleted();
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
    void testForgettingSweepExpiresStaleAndSparesImportant();
    void testSqliteEmbeddingIndexSearch();
    void testModelDownloaderLocalMirror();
    void testTransactionRollbackRevertsWrites();
    void testTransactionCommitRetainsWrites();
    void testTransactionRollbackRevertsRelationGraph();
    void testDaydreamDrainUpgradesAndClearsHippocampus();
    void testDaydreamDrainDiscardsLowValue();
    void testDaydreamDrainSparesOtherPartitions();
    void testStoreKeyPersistsRoundtrip();
    void testDaydreamDrainUpgradesViaPersistedMentionCount();
    void testDaydreamTriggerPolicyAllConditions();
    void testDaydreamTriggerPolicyNegativeCases();
    void testDaydreamTriggerPolicyNoDueTodoNonBlocking();
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

    // addEntry 返回派生 partition 后的副本
    QCOMPARE(storedSemantic.partition, QStringLiteral("semantic"));
    QCOMPARE(storedWorking.partition, QStringLiteral("hippocampus"));

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
    QVERIFY(all.size() >= 2);
    bool foundSemantic = false, foundWorking = false;
    for (const MemoryEntry& e : all) {
        if (e.type == MemoryType::Semantic) {
            QCOMPARE(e.partition, QStringLiteral("semantic"));
            foundSemantic = true;
        }
        if (e.type == MemoryType::ShortTerm) {
            QCOMPARE(e.partition, QStringLiteral("hippocampus"));
            foundWorking = true;
        }
    }
    QVERIFY(foundSemantic);
    QVERIFY(foundWorking);
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

// 模型下载器：用本地 file:// 镜像验证下载/跳过/sha 校验，不依赖外网 HF。
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
    hot.source = QStringLiteral("assistant_response");
    hot.importance = 0.4;
    hot.mentionCount = 3;
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

// 回归 review finding #1：生产路径写 ShortTerm 必须把 cache 的 recurrence 计数
// 持久化进 MemoryEntry.mentionCount，否则 Daydream drain 100% discard。此测试用
// 修复后的等价写入路径（cache.add 两次同 summary → countMentions → 写 ShortTerm），
// 验证 mentionCount 持久化=2、drain 升级为 Episodic、privacy=Personal。
void TestMemoryStrategy::testDaydreamDrainUpgradesViaPersistedMentionCount() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    MemoryStore store;
    setupStoreWithDb(store, tempDir);

    // 模拟修复后的 rememberAssistantResponse：先 cache.add 自增，再带 countMentions 写 ShortTerm。
    WorkingMemoryCache cache;
    WorkingMemoryItem wm;
    wm.summary = QStringLiteral("面试安排");
    wm.content = QStringLiteral("用户反复提到的面试安排");
    wm.tags = {QStringLiteral("user_request"), QStringLiteral("assistant")};
    wm.source = QStringLiteral("assistant_response");
    wm.importance = 0.3;
    cache.add(wm);
    cache.add(wm); // 同 summary → mentionCount 自增到 2

    MemoryEntry shortTerm;
    shortTerm.type = MemoryType::ShortTerm;
    shortTerm.key = QStringLiteral("assistant_response");
    shortTerm.value = wm.content;
    shortTerm.tags = wm.tags;
    shortTerm.status = MemoryStatus::Active;
    shortTerm.privacyLevel = PrivacyLevel::Public;
    shortTerm.source = QStringLiteral("assistant_inferred");
    shortTerm.confidence = 0.4;
    shortTerm.importance = 0.2;
    shortTerm.strength = 0.2;
    shortTerm.mentionCount = cache.countMentions(wm.summary); // =2
    store.addEntry(shortTerm);
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
    QCOMPARE(upgraded.source, QStringLiteral("consolidation"));
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
