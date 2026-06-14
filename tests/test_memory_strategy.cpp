//
// Memory extractor / policy / retriever / relation tests
//

#include <QTemporaryDir>
#include <QTest>

#include "memory/memory_extractor.h"
#include "memory/memory_policy.h"
#include "memory/memory_relation.h"
#include "memory/memory_relation_graph.h"
#include "memory/memory_retriever.h"
#include "memory/memory_store.h"
#include "memory/working_memory_cache.h"
#include "memory/noop_embedding_index.h"

class TestMemoryStrategy : public QObject {
    Q_OBJECT

private slots:
    void testExtractorCreatesPreferenceFromLikeStatement();
    void testExtractorCreatesSemanticFromRememberStatement();
    void testExtractorCreatesForgetCandidate();
    void testPolicyWritesAndSkipsDuplicate();
    void testPolicyRejectsSensitiveMemory();
    void testPolicyMarksMatchedMemoryDeleted();
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
    void testRetrieverGraphExpansion();
    void testWorkingMemoryCacheTtl();
    void testWorkingMemoryCacheCapacity();
    void testWorkingMemoryCacheConsolidation();
    void testWorkingMemoryRetrieverIntegration();
    void testNoopEmbeddingIndexDoesNotAffectRetrieval();

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

    // Old memory should be superseded
    const MemoryEntry* old = store.findById(oldId);
    QVERIFY(old);
    QCOMPARE(old->status, MemoryStatus::Superseded);

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

QTEST_MAIN(TestMemoryStrategy)
#include "test_memory_strategy.moc"
