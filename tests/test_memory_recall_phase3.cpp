#include <QtTest>
#include <algorithm>
#include <cmath>
#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include "core/ai/memory/sqlite_memory_repository.h"
#include "core/ai/memory/working_memory_cache.h"
#include "core/ai/memory/associative_activation_engine.h"
#include "core/ai/memory/memory_relation_graph.h"
#include "core/ai/memory/tag_cooccurrence_graph.h"
#include "core/ai/memory/memory_store.h"
#include "core/ai/memory/memory_retriever.h"
#include "core/ai/memory/active_memory_pool.h"
#include "core/ai/memory/memory_keyword_index.h"
#include "core/ai/memory/embedding_index.h"
#include "core/ai/memory/hippocampus_working_set.h"
#include "core/ai/memory/hybrid_graph_builder.h"

class TestMemoryRecallPhase3 : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testExpiredActiveEntriesCannotRecall();
    void testForbiddenIntermediate_data();
    void testForbiddenIntermediate();
    void testNonTraversableEdgesDoNotConsumeBudget();
    void testDefaultBuilderEdgesPropagate_data();
    void testDefaultBuilderEdgesPropagate();
    void testHippocampusScoresSurviveSeedPruning();
    void testPropagationOneHop();
    void testPropagationTwoHops();
    void testLoopPrevention();
    void testMinDeltaThreshold();
    void testCandidateBudget();
    void testSupersedesNotDiffused();
    void testExplorationDeterministicWithSeed();
    void testExplorationRateBounds();
    void testPersonalityModulation();
    void testGraphRetrievalIntegration();
    void testUnconnectedSeedsHaveNoGraphEvidence_data();
    void testUnconnectedSeedsHaveNoGraphEvidence();
    void testSeedsReceiveOnlyActualEdgeContributions();
    void testSeedBudgetPreservesMultiChannelMatch();
    void testFullCandidatePoolReachesRanker();
    void testExplorationSelectionBudget();
    void testExplorationReachesRetrievalOutput();
    void testConvergingPathsAccumulateAtCapacity();
    void testHippocampusSeedBudgetIgnoresEmotionAndImportance();
};

namespace {

// Fixed random source for reproducible exploration tests
std::function<double()> fixedRandomSource(double value) {
    return [value]() { return value; };
}

MemoryRelation makeRelation(const QString& id,
                            const QString& from,
                            const QString& to,
                            MemoryRelationType type,
                            double weight = 1.0,
                            double confidence = 1.0) {
    MemoryRelation rel;
    rel.id = id;
    rel.fromMemoryId = from;
    rel.toMemoryId = to;
    rel.type = type;
    rel.weight = weight;
    rel.confidence = confidence;
    rel.createdAt = QDateTime::currentDateTimeUtc();
    return rel;
}

}

void TestMemoryRecallPhase3::testHippocampusSeedBudgetIgnoresEmotionAndImportance() {
    MemoryStore store;
    const auto now = QDateTime::currentDateTimeUtc();
    for (int i = 0; i < 10; ++i) {
        MemoryEntry entry;
        entry.id = QString("seed-%1").arg(i);
        entry.type = MemoryType::ShortTerm;
        entry.summary = i < 8 ? "music jazz" : "jazz";
        entry.importance = i < 8 ? 0.1 : 1.0;
        entry.mentionCount = i < 8 ? 1 : 100;
        entry.emotion = EmotionType::Joy;
        entry.emotionConfidence = 1.0;
        entry.emotionIntensity = i < 8 ? 0.0 : 1.0;
        entry.createdAt = entry.updatedAt = now.addSecs(i < 8 ? -86400 : 0);
        QVERIFY(!store.addEntry(entry).id.isEmpty());
    }
    HippocampusWorkingSet workingSet(&store);
    QVERIFY(workingSet.refresh());
    AssociativeActivationEngine graph;
    graph.setRandomSource(fixedRandomSource(0.99));
    ActivationChannels channels;
    channels.workingSet = &workingSet;
    channels.graphPropagation = &graph;
    MemoryQuery query;
    query.text = "jazz music";
    query.limit = 16;
    MemoryRetriever retriever;
    const auto neutral = retriever.retrieveWithGraphPropagation(store, query, channels, nullptr, true);
    query.currentEmotion = EmotionType::Joy;
    query.currentEmotionIntensity = 1.0;
    const auto happy = retriever.retrieveWithGraphPropagation(store, query, channels, nullptr, true);
    QCOMPARE(neutral.size(), 8);
    QCOMPARE(happy.size(), 8);
    for (int i = 0; i < happy.size(); ++i) {
        QCOMPARE(happy[i].entry.id, neutral[i].entry.id);
        QVERIFY(happy[i].entry.id != "seed-8" && happy[i].entry.id != "seed-9");
        QCOMPARE(happy[i].scoreWithoutEmotion, neutral[i].score);
        QVERIFY(qAbs(happy[i].score - neutral[i].score - 0.3) < 1e-12);
    }
}

void TestMemoryRecallPhase3::testSeedBudgetPreservesMultiChannelMatch() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath(QStringLiteral("seeds.db")));
    QVERIFY(store.loadDatabaseOnly());
    class FixedIndex final : public EmbeddingIndex {
    public:
        QList<EmbeddingSearchResult> hits;
        bool upsert(const QString&, const QString&) override { return false; }
        bool remove(const QString&) override { return false; }
        QList<EmbeddingSearchResult> search(const QString&, int limit) override { return hits.mid(0, limit); }
    } index;
    for (int i = 0; i < 20; ++i) {
        MemoryEntry entry;
        entry.type = MemoryType::Semantic;
        entry.key = QStringLiteral("distractor-%1").arg(i);
        entry.summary = QStringLiteral("unrelated event %1").arg(i);
        const auto saved = store.addEntry(entry);
        QVERIFY(!saved.id.isEmpty());
        index.hits.append({saved.id, 0.9});
    }
    MemoryEntry target;
    target.type = MemoryType::Semantic;
    target.key = QStringLiteral("needle");
    target.summary = QStringLiteral("needle");
    target = store.addEntry(target);
    QVERIFY(!target.id.isEmpty());
    index.hits.append({target.id, 0.2});
    MemoryKeywordIndex keywords;
    keywords.rebuild(store.all());
    ActivationChannels channels;
    channels.keywordIndex = &keywords;
    channels.embeddingIndex = &index;
    MemoryQuery query;
    query.text = QStringLiteral("needle");
    query.limit = 16;
    const auto results = MemoryRetriever().retrieveWithGraphPropagation(
        store, query, channels, nullptr, true);
    bool found = false;
    for (const auto& result : results) {
        if (result.entry.id != target.id) continue;
        found = true;
        QVERIFY(result.sourceChannels.contains(QStringLiteral("keyword")));
        QVERIFY(result.sourceChannels.contains(QStringLiteral("embedding")));
    }
    QVERIFY(found);
}

void TestMemoryRecallPhase3::testFullCandidatePoolReachesRanker() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath("full-pool.db"));
    QVERIFY(store.loadDatabaseOnly());
    ActiveMemoryPool pool;
    QStringList seedIds;
    for (int i = 0; i < 4; ++i) {
        MemoryEntry seed;
        seed.type = MemoryType::Semantic;
        seed.key = QString("seed-%1").arg(i);
        seed = store.addEntry(seed);
        pool.activate(seed.id, 2.0, "session");
        seedIds.append(seed.id);
    }
    QString winner;
    for (int i = 0; i < 60; ++i) {
        MemoryEntry entry;
        entry.type = MemoryType::Semantic;
        entry.key = QString("neighbor-%1").arg(i);
        entry.strength = i == 59 ? 1.0 : 0.0;
        entry.importance = i == 59 ? 1.0 : 0.0;
        entry = store.addEntry(entry);
        QVERIFY(!entry.id.isEmpty());
        if (i == 59) winner = entry.id;
        QVERIFY(store.relationGraph().addRelation(makeRelation(
            QString("edge-%1").arg(i), seedIds.at(i / 15), entry.id,
            MemoryRelationType::Related, i == 59 ? 0.1 : 1.0)));
    }
    AssociativeActivationEngine engine;
    engine.setMinPropagationDelta(0.0);
    engine.setRandomSource(fixedRandomSource(0.99));
    ActivationChannels channels;
    channels.activePool = &pool;
    channels.graphPropagation = &engine;
    MemoryQuery query;
    query.limit = 64;
    MemoryRetriever retriever;
    const auto all = retriever.retrieveWithGraphPropagation(store, query, channels, nullptr, true);
    QCOMPARE(all.size(), 64);
    class FixedIndex final : public EmbeddingIndex {
    public:
        QList<EmbeddingSearchResult> hits;
        bool upsert(const QString&, const QString&) override { return false; }
        bool remove(const QString&) override { return false; }
        QList<EmbeddingSearchResult> search(const QString&, int limit) override { return hits.mid(0, limit); }
    } index;
    for (const auto& item : all) index.hits.append({item.entry.id, 0.8});
    ActivationChannels semanticOnly;
    semanticOnly.embeddingIndex = &index;
    MemoryQuery semanticQuery;
    semanticQuery.text = "query";
    semanticQuery.limit = 64;
    QCOMPARE(retriever.retrieveActivated(store, semanticQuery, semanticOnly).size(), 32);
    query.limit = 8;
    const auto top = retriever.retrieveWithGraphPropagation(store, query, channels, nullptr, true);
    QCOMPARE(top.size(), 8);
    bool found = false;
    for (const auto& item : top) if (item.entry.id == winner) found = true;
    QVERIFY2(found, "Low graph activation but strong ACT-R evidence must survive to ranking");
}

void TestMemoryRecallPhase3::testExplorationSelectionBudget() {
    QList<CandidateMemory> candidates;
    for (int i = 0; i < 12; ++i) {
        CandidateMemory candidate;
        candidate.entry.id = QString::number(i);
        candidate.runtimeActivation = 2.0;
        candidates.append(candidate);
    }
    for (int i = 0; i < 3; ++i) {
        CandidateMemory candidate;
        candidate.entry.id = QString("explore-%1").arg(i);
        candidate.isExploratory = true;
        candidate.graphActivation = 0.01 * (i + 1);
        candidates.append(candidate);
    }
    ACTRRanker ranker;
    for (int limit : {1, 4, 8}) {
        const auto selected = ranker.select(candidates, {}, limit);
        QCOMPARE(selected.size(), limit);
        int exploratory = 0;
        for (const auto& candidate : selected) exploratory += candidate.isExploratory;
        QCOMPARE(exploratory, 1);
        QCOMPARE(selected.last().entry.id, QString("explore-2"));
        for (int i = 1; i < selected.size() - 1; ++i)
            QVERIFY(selected[i-1].finalScore >= selected[i].finalScore);
    }
    // Even when exploration dominates scores, it cannot occupy multiple slots.
    for (auto& candidate : candidates)
        if (candidate.isExploratory) candidate.runtimeActivation = 20.0;
    const auto high = ranker.select(candidates, {}, 8);
    QCOMPARE(high.size(), 8);
    int count = 0;
    for (const auto& candidate : high) count += candidate.isExploratory;
    QCOMPARE(count, 1);
    candidates = candidates.mid(0, 12);
    QCOMPARE(ranker.select(candidates, {}, 8).size(), 8);
    QVERIFY(ranker.select(candidates, {}, 0).isEmpty());
}

void TestMemoryRecallPhase3::testExplorationReachesRetrievalOutput() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath("exploration.db"));
    QVERIFY(store.loadDatabaseOnly());
    ActiveMemoryPool pool;
    QString seedId;
    for (int i = 0; i < 12; ++i) {
        MemoryEntry seed;
        seed.type = MemoryType::Semantic;
        seed.key = QString("seed-%1").arg(i);
        seed = store.addEntry(seed);
        pool.activate(seed.id, 2.0, "session");
        seedId = seed.id;
    }
    MemoryEntry target;
    target.type = MemoryType::Semantic;
    target.key = "exploration-target";
    target = store.addEntry(target);
    QVERIFY(store.relationGraph().addRelation(makeRelation(
        "explore-edge", seedId, target.id, MemoryRelationType::Related)));
    AssociativeActivationEngine engine;
    engine.setRandomSource(fixedRandomSource(0.99));
    ActivationChannels channels;
    channels.activePool = &pool;
    channels.graphPropagation = &engine;
    MemoryRetriever retriever;
    MemoryQuery query;
    const auto stable = retriever.retrieveWithGraphPropagation(store, query, channels, nullptr, true);
    for (const auto& item : stable) QVERIFY(item.entry.id != target.id);
    engine.setRandomSource(fixedRandomSource(0.05));
    const auto explored = retriever.retrieveWithGraphPropagation(store, query, channels, nullptr, true);
    QCOMPARE(explored.size(), 8);
    QCOMPARE(explored.last().entry.id, target.id);
    QVERIFY(explored.last().isExploratory);
    QVERIFY(explored.last().fromGraphExpansion);
    QVERIFY(explored.last().sourceChannels.contains("graph_exploratory"));
    target.privacyLevel = PrivacyLevel::Sensitive;
    QVERIFY(store.updateEntryById(target));
    const auto filtered = retriever.retrieveWithGraphPropagation(store, query, channels, nullptr, true);
    QCOMPARE(filtered.size(), 8);
    for (const auto& item : filtered) QVERIFY(item.entry.id != target.id);
}

void TestMemoryRecallPhase3::testConvergingPathsAccumulateAtCapacity() {
    MemoryRelationGraph graph;
    graph.setConnectionName("phase3_test_conn");
    QSqlQuery query(QSqlDatabase::database("phase3_test_conn"));
    QVERIFY(query.exec("DELETE FROM memory_relations"));
    QVERIFY(graph.addRelation(makeRelation("a", "seed", "left", MemoryRelationType::DerivedFrom)));
    QVERIFY(graph.addRelation(makeRelation("b", "seed", "right", MemoryRelationType::DerivedFrom)));
    QVERIFY(graph.addRelation(makeRelation("c", "left", "target", MemoryRelationType::DerivedFrom)));
    QVERIFY(graph.addRelation(makeRelation("d", "right", "target", MemoryRelationType::DerivedFrom)));
    AssociativeActivationEngine engine;
    engine.setMaxCandidates(4);
    engine.setRandomSource(fixedRandomSource(0.99));
    const double first = (1.0 / (1.0 + std::exp(-1.0))) * 0.55 / std::sqrt(2.0);
    // Only target remains traversable after removing the return to seed.
    const double second = (1.0 / (1.0 + std::exp(-first))) * 0.55 * 0.55;
    const auto result = engine.propagate({{"seed", 1.0}}, graph);
    QCOMPARE(result.size(), 4);
    bool found = false;
    for (const auto& item : result) {
        if (item.memoryId == "seed") {
            QCOMPARE(item.seedActivation, 1.0);
            QCOMPARE(item.activation, 0.0);
        }
        if (item.memoryId != "target") continue;
        found = true;
        QVERIFY(std::abs(item.activation - 2.0 * second) < 1e-9);
        QCOMPARE(item.hopCount, 2);
        QCOMPARE(item.propagationPath.size(), 3);
    }
    QVERIFY(found);
    // Convergence before the final hop must forward each path only once.
    QVERIFY(query.exec("DELETE FROM memory_relations"));
    QVERIFY(graph.addRelation(makeRelation("e", "seed-a", "mid", MemoryRelationType::DerivedFrom)));
    QVERIFY(graph.addRelation(makeRelation("f", "seed-b", "mid", MemoryRelationType::DerivedFrom)));
    QVERIFY(graph.addRelation(makeRelation("g", "mid", "leaf", MemoryRelationType::DerivedFrom)));
    const double firstA = (1.0 / (1.0 + std::exp(-1.0))) * 0.55;
    const double firstB = (1.0 / (1.0 + std::exp(-0.4))) * 0.55;
    const double expectedLeaf = (1.0 / (1.0 + std::exp(-firstA))
        + 1.0 / (1.0 + std::exp(-firstB))) * 0.55 * 0.55 / std::sqrt(2.0);
    const auto merged = engine.propagate({{"seed-a", 1.0}, {"seed-b", 0.4}}, graph);
    QCOMPARE(merged.size(), 4);
    bool leafFound = false;
    for (const auto& item : merged) {
        if (item.memoryId == "mid") QVERIFY(std::abs(item.activation - firstA - firstB) < 1e-9);
        if (item.memoryId != "leaf") continue;
        leafFound = true;
        QVERIFY(std::abs(item.activation - expectedLeaf) < 1e-9);
    }
    QVERIFY(leafFound);
}

void TestMemoryRecallPhase3::testUnconnectedSeedsHaveNoGraphEvidence_data() {
    QTest::addColumn<bool>("useActivePool");
    QTest::newRow("keyword-seed") << false;
    QTest::newRow("active-pool-seed") << true;
}

void TestMemoryRecallPhase3::testUnconnectedSeedsHaveNoGraphEvidence() {
    QFETCH(bool, useActivePool);
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath("unconnected-seed.db"));
    QVERIFY(store.loadDatabaseOnly());
    MemoryEntry seed;
    seed.type = MemoryType::Semantic;
    seed.summary = "needle";
    seed.importance = seed.strength = 0.6;
    seed = store.addEntry(seed);
    QVERIFY(!seed.id.isEmpty());
    MemoryKeywordIndex keywords;
    keywords.rebuild(store.all());
    ActiveMemoryPool pool;
    pool.activate(seed.id, 1.2, "session");
    AssociativeActivationEngine engine;
    engine.setRandomSource(fixedRandomSource(0.99));
    const auto propagated = engine.propagate({{seed.id, 1.2}}, store.relationGraph());
    QCOMPARE(propagated.size(), 1);
    QCOMPARE(propagated.first().seedActivation, 1.2);
    QCOMPARE(propagated.first().activation, 0.0);

    ActivationChannels channels;
    channels.keywordIndex = &keywords;
    if (useActivePool) channels.activePool = &pool;
    MemoryQuery query;
    query.text = "needle";
    MemoryRetriever retriever;
    const auto withoutGraph = retriever.retrieveWithGraphPropagation(
        store, query, channels, nullptr, true);
    channels.graphPropagation = &engine;
    const auto withGraph = retriever.retrieveWithGraphPropagation(
        store, query, channels, nullptr, true);
    QCOMPARE(withoutGraph.size(), 1);
    QCOMPARE(withGraph.size(), 1);
    QCOMPARE(withGraph.first().score, withoutGraph.first().score);
    QCOMPARE(withGraph.first().scoreWithoutEmotion, withoutGraph.first().scoreWithoutEmotion);
    QCOMPARE(withGraph.first().runtimeActivation, useActivePool ? 0.6 : 0.0);
    QVERIFY(!withGraph.first().fromGraphExpansion);
    QVERIFY(!withGraph.first().sourceChannels.contains("graph_propagation"));
    QVERIFY(!withGraph.first().sourceChannels.contains("graph_exploratory"));
    for (const auto& reason : withGraph.first().reasons)
        QVERIFY(!reason.startsWith("graph_path:"));
}

void TestMemoryRecallPhase3::testSeedsReceiveOnlyActualEdgeContributions() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath("connected-seeds.db"));
    QVERIFY(store.loadDatabaseOnly());
    MemoryEntry first;
    first.type = MemoryType::Semantic;
    first.key = "first";
    first.summary = "needle";
    first = store.addEntry(first);
    MemoryEntry second = first;
    second.id.clear();
    second.key = "second";
    second = store.addEntry(second);
    QVERIFY(!first.id.isEmpty());
    QVERIFY(!second.id.isEmpty());
    QVERIFY(first.id != second.id);
    QVERIFY(store.relationGraph().addRelation(makeRelation(
        "seed-link", first.id, second.id, MemoryRelationType::DerivedFrom)));
    ActiveMemoryPool pool;
    pool.activate(first.id, 0.4, "session");
    pool.activate(second.id, 1.2, "session");
    MemoryKeywordIndex keywords;
    keywords.rebuild(store.all());
    AssociativeActivationEngine engine;
    engine.setRandomSource(fixedRandomSource(0.99));
    const double contributionToFirst = 0.55 / (1.0 + std::exp(-1.2));
    const double contributionToSecond = 0.55 / (1.0 + std::exp(-0.4));
    const auto propagated = engine.propagate(
        {{first.id, 0.4}, {second.id, 1.2}}, store.relationGraph());
    QCOMPARE(propagated.size(), 2);
    for (const auto& item : propagated) {
        const bool isFirst = item.memoryId == first.id;
        QCOMPARE(item.seedActivation, isFirst ? 0.4 : 1.2);
        QVERIFY(qAbs(item.activation - (isFirst ? contributionToFirst : contributionToSecond)) < 1e-12);
        QCOMPARE(item.hopCount, 1);
        QCOMPARE(item.propagationPath, QStringList({isFirst ? second.id : first.id, item.memoryId}));
    }

    ActivationChannels channels;
    channels.activePool = &pool;
    channels.keywordIndex = &keywords;
    MemoryQuery query;
    query.text = "needle";
    MemoryRetriever retriever;
    const auto withoutGraph = retriever.retrieveWithGraphPropagation(
        store, query, channels, nullptr, true);
    channels.graphPropagation = &engine;
    const auto withGraph = retriever.retrieveWithGraphPropagation(
        store, query, channels, nullptr, true);
    QCOMPARE(withoutGraph.size(), 2);
    QCOMPARE(withGraph.size(), 2);
    for (const auto& item : withGraph) {
        const auto baseline = std::find_if(withoutGraph.cbegin(), withoutGraph.cend(),
            [&](const RetrievedMemory& other) { return other.entry.id == item.entry.id; });
        QVERIFY(baseline != withoutGraph.cend());
        const double contribution = item.entry.id == first.id ? contributionToFirst : contributionToSecond;
        QVERIFY(qAbs(item.score - baseline->score - 0.6 * contribution) < 1e-12);
        QCOMPARE(item.runtimeActivation, baseline->runtimeActivation);
        QVERIFY(item.sourceChannels.contains("graph_propagation"));
        QVERIFY(!item.fromGraphExpansion);
    }
    // An explored path must not turn an independently retrieved seed into an
    // exploration-only candidate and consume the sole exploratory result slot.
    engine.setRandomSource(fixedRandomSource(0.05));
    const auto explored = retriever.retrieveWithGraphPropagation(
        store, query, channels, nullptr, true);
    QCOMPARE(explored.size(), 2);
    for (const auto& item : explored) QVERIFY(!item.isExploratory);
}

void TestMemoryRecallPhase3::initTestCase() {
    QVERIFY(m_tempDir.isValid());
    m_dbPath = m_tempDir.filePath("test_phase3_relations.db");
    SQLiteMemoryRepository repository;
    QVERIFY(repository.open(m_dbPath));
    repository.close();

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "phase3_test_conn");
    db.setDatabaseName(m_dbPath);
    QVERIFY(db.open());
}

void TestMemoryRecallPhase3::cleanupTestCase() {
    {
        QSqlDatabase db = QSqlDatabase::database("phase3_test_conn", false);
        if (db.isOpen()) db.close();
    }
    QSqlDatabase::removeDatabase("phase3_test_conn");
    QFile::remove(m_dbPath);
}

void TestMemoryRecallPhase3::testPropagationOneHop() {
    MemoryRelationGraph graph;
    graph.setConnectionName("phase3_test_conn");

    QSqlDatabase db = QSqlDatabase::database("phase3_test_conn");
    QSqlQuery q(db);
    q.exec("DELETE FROM memory_relations");
    QVERIFY(graph.addRelation(makeRelation(
        "r1", "seed1", "target1", MemoryRelationType::Related)));

    AssociativeActivationEngine engine;
    engine.setRandomSource(fixedRandomSource(0.99));  // Never explore (rate < 0.99)

    QHash<QString, double> seeds;
    seeds["seed1"] = 1.0;

    QList<PropagatedMemory> results = engine.propagate(seeds, graph);

    // Should contain seed + target1
    bool foundSeed = false, foundTarget = false;
    for (const PropagatedMemory& mem : results) {
        if (mem.memoryId == "seed1") { foundSeed = true; QCOMPARE(mem.hopCount, 0); }
        if (mem.memoryId == "target1") {
            foundTarget = true;
            QCOMPARE(mem.hopCount, 1);
            QVERIFY(mem.activation > 0.0);
            QCOMPARE(mem.propagationPath, QStringList({"seed1", "target1"}));
        }
    }
    QVERIFY(foundSeed);
    QVERIFY(foundTarget);
}

void TestMemoryRecallPhase3::testPropagationTwoHops() {
    MemoryRelationGraph graph;
    graph.setConnectionName("phase3_test_conn");

    QSqlDatabase db = QSqlDatabase::database("phase3_test_conn");
    QSqlQuery q(db);
    q.exec("DELETE FROM memory_relations");
    QVERIFY(graph.addRelation(makeRelation(
        "r1", "seed1", "hop1", MemoryRelationType::DerivedFrom)));
    QVERIFY(graph.addRelation(makeRelation(
        "r2", "hop1", "hop2", MemoryRelationType::DerivedFrom)));
    QVERIFY(graph.addRelation(makeRelation(
        "r3", "hop2", "hop3", MemoryRelationType::DerivedFrom)));  // 3rd hop, should NOT propagate

    AssociativeActivationEngine engine;
    engine.setRandomSource(fixedRandomSource(0.99));

    QHash<QString, double> seeds;
    seeds["seed1"] = 1.0;

    QList<PropagatedMemory> results = engine.propagate(seeds, graph);

    QStringList foundIds;
    for (const PropagatedMemory& mem : results) {
        foundIds.append(mem.memoryId);
    }

    QVERIFY(foundIds.contains("seed1"));
    QVERIFY(foundIds.contains("hop1"));
    QVERIFY(foundIds.contains("hop2"));
    QVERIFY(!foundIds.contains("hop3"));  // Beyond max 2 hops
}

void TestMemoryRecallPhase3::testLoopPrevention() {
    MemoryRelationGraph graph;
    graph.setConnectionName("phase3_test_conn");

    QSqlDatabase db = QSqlDatabase::database("phase3_test_conn");
    QSqlQuery q(db);
    q.exec("DELETE FROM memory_relations");
    // Cycle: a -> b -> c -> a
    QVERIFY(graph.addRelation(makeRelation(
        "r1", "a", "b", MemoryRelationType::Related)));
    QVERIFY(graph.addRelation(makeRelation(
        "r2", "b", "c", MemoryRelationType::Related)));
    QVERIFY(graph.addRelation(makeRelation(
        "r3", "c", "a", MemoryRelationType::Related)));

    AssociativeActivationEngine engine;
    engine.setRandomSource(fixedRandomSource(0.99));

    QHash<QString, double> seeds;
    seeds["a"] = 1.0;

    QList<PropagatedMemory> results = engine.propagate(seeds, graph);

    // Each node appears exactly once (no infinite loop)
    QSet<QString> ids;
    for (const PropagatedMemory& mem : results) {
        QVERIFY(!ids.contains(mem.memoryId));
        ids.insert(mem.memoryId);
    }
    QCOMPARE(ids.size(), 3);  // a, b, c
}

void TestMemoryRecallPhase3::testMinDeltaThreshold() {
    MemoryRelationGraph graph;
    graph.setConnectionName("phase3_test_conn");

    QSqlDatabase db = QSqlDatabase::database("phase3_test_conn");
    QSqlQuery q(db);
    q.exec("DELETE FROM memory_relations");
    // Weak edge: low weight + low confidence -> delta below 0.01
    QVERIFY(graph.addRelation(makeRelation(
        "r1", "seed1", "weak_target", MemoryRelationType::MentionedWith, 0.1, 0.2)));
    // Strong edge: should propagate
    QVERIFY(graph.addRelation(makeRelation(
        "r2", "seed1", "strong_target", MemoryRelationType::DerivedFrom, 1.0, 1.0)));

    AssociativeActivationEngine engine;
    engine.setRandomSource(fixedRandomSource(0.99));

    QHash<QString, double> seeds;
    seeds["seed1"] = 1.0;

    QList<PropagatedMemory> results = engine.propagate(seeds, graph);

    QStringList foundIds;
    for (const PropagatedMemory& mem : results) {
        foundIds.append(mem.memoryId);
    }

    QVERIFY(foundIds.contains("strong_target"));
    QVERIFY(!foundIds.contains("weak_target"));  // Below min delta 0.01
}

void TestMemoryRecallPhase3::testCandidateBudget() {
    MemoryRelationGraph graph;
    graph.setConnectionName("phase3_test_conn");

    QSqlDatabase db = QSqlDatabase::database("phase3_test_conn");
    QSqlQuery q(db);
    q.exec("DELETE FROM memory_relations");

    // Create 100 neighbors of seed1
    for (int i = 0; i < 100; ++i) {
        QVERIFY(graph.addRelation(makeRelation(
            QString("r%1").arg(i), "seed1", QString("n%1").arg(i),
            MemoryRelationType::Related, 1.0, 1.0)));
    }

    AssociativeActivationEngine engine;
    engine.setMaxCandidates(10);  // Tight budget for test
    engine.setRandomSource(fixedRandomSource(0.99));

    QHash<QString, double> seeds;
    seeds["seed1"] = 1.0;

    QList<PropagatedMemory> results = engine.propagate(seeds, graph);

    QVERIFY(results.size() <= 10);
}

void TestMemoryRecallPhase3::testSupersedesNotDiffused() {
    MemoryRelationGraph graph;
    graph.setConnectionName("phase3_test_conn");

    QSqlDatabase db = QSqlDatabase::database("phase3_test_conn");
    QSqlQuery q(db);
    q.exec("DELETE FROM memory_relations");
    QVERIFY(graph.addRelation(makeRelation(
        "r1", "old1", "new1", MemoryRelationType::Supersedes)));

    AssociativeActivationEngine engine;
    engine.setRandomSource(fixedRandomSource(0.99));

    QHash<QString, double> seeds;
    seeds["old1"] = 1.0;

    QList<PropagatedMemory> results = engine.propagate(seeds, graph);

    QStringList foundIds;
    for (const PropagatedMemory& mem : results) {
        foundIds.append(mem.memoryId);
    }

    QCOMPARE(foundIds.size(), 1);  // Only seed, no propagation
    QVERIFY(foundIds.contains("old1"));
    QVERIFY(!foundIds.contains("new1"));  // Supersedes not diffused
}

void TestMemoryRecallPhase3::testExplorationDeterministicWithSeed() {
    MemoryRelationGraph graph;
    graph.setConnectionName("phase3_test_conn");

    QSqlDatabase db = QSqlDatabase::database("phase3_test_conn");
    QSqlQuery q(db);
    q.exec("DELETE FROM memory_relations");
    QVERIFY(graph.addRelation(makeRelation(
        "r1", "seed1", "t1", MemoryRelationType::Related)));
    QVERIFY(graph.addRelation(makeRelation(
        "r2", "seed1", "t2", MemoryRelationType::Related)));

    AssociativeActivationEngine engine;
    engine.setPersonality({0.8, 0.5, 0.5});  // High openness -> more exploration
    engine.setRandomSource(fixedRandomSource(0.05));  // Always explore

    QHash<QString, double> seeds;
    seeds["seed1"] = 1.0;

    // Two runs with same seed should produce identical results
    QList<PropagatedMemory> run1 = engine.propagate(seeds, graph);
    QList<PropagatedMemory> run2 = engine.propagate(seeds, graph);

    QCOMPARE(run1.size(), run2.size());
    for (int i = 0; i < run1.size(); ++i) {
        QCOMPARE(run1[i].memoryId, run2[i].memoryId);
        QCOMPARE(run1[i].isExploratory, run2[i].isExploratory);
    }

    // With always-explore random source, at least some nodes marked exploratory
    bool hasExploratory = false;
    for (const PropagatedMemory& mem : run1) {
        if (mem.isExploratory) { hasExploratory = true; break; }
    }
    QVERIFY(hasExploratory);
}

void TestMemoryRecallPhase3::testExplorationRateBounds() {
    // explorationRate = clamp(0.10 + 0.15 * openness, 0.10, 0.25)
    // openness = 0.0 -> 0.10; openness = 0.5 -> 0.175; openness = 1.0 -> 0.25
    // 边界通过行为验证：random = 0.26 时即使 openness = 1.0 也不探索
    MemoryRelationGraph graph;
    graph.setConnectionName("phase3_test_conn");

    QSqlDatabase db = QSqlDatabase::database("phase3_test_conn");
    QSqlQuery q(db);
    q.exec("DELETE FROM memory_relations");
    QVERIFY(graph.addRelation(makeRelation(
        "rb1", "seed1", "t1", MemoryRelationType::Related)));
    QVERIFY(graph.addRelation(makeRelation(
        "rb2", "seed1", "t2", MemoryRelationType::Related)));

    AssociativeActivationEngine engine;
    engine.setPersonality({1.0, 0.5, 0.5});  // Max openness -> rate = 0.25
    engine.setRandomSource(fixedRandomSource(0.26));  // Above max rate -> never explore

    QHash<QString, double> seeds;
    seeds["seed1"] = 1.0;

    QList<PropagatedMemory> results = engine.propagate(seeds, graph);

    for (const PropagatedMemory& mem : results) {
        QVERIFY(!mem.isExploratory);  // rate capped at 0.25 < 0.26
    }
}

void TestMemoryRecallPhase3::testPersonalityModulation() {
    MemoryRelationGraph graph;
    graph.setConnectionName("phase3_test_conn");

    QSqlDatabase db = QSqlDatabase::database("phase3_test_conn");
    QSqlQuery q(db);
    q.exec("DELETE FROM memory_relations");
    QVERIFY(graph.addRelation(makeRelation(
        "r1", "seed1", "social1", MemoryRelationType::MentionedWith, 1.0, 1.0)));

    // Neutral personality
    AssociativeActivationEngine neutralEngine;
    neutralEngine.setRandomSource(fixedRandomSource(0.99));
    neutralEngine.setPersonality({0.5, 0.5, 0.5});

    // High sociability
    AssociativeActivationEngine sociableEngine;
    sociableEngine.setRandomSource(fixedRandomSource(0.99));
    sociableEngine.setPersonality({0.5, 1.0, 0.5});

    QHash<QString, double> seeds;
    seeds["seed1"] = 1.0;

    QList<PropagatedMemory> neutralResults = neutralEngine.propagate(seeds, graph);
    QList<PropagatedMemory> sociableResults = sociableEngine.propagate(seeds, graph);

    double neutralActivation = 0.0, sociableActivation = 0.0;
    for (const PropagatedMemory& mem : neutralResults) {
        if (mem.memoryId == "social1") neutralActivation = mem.activation;
    }
    for (const PropagatedMemory& mem : sociableResults) {
        if (mem.memoryId == "social1") sociableActivation = mem.activation;
    }

    // High sociability should boost MentionedWith propagation
    QVERIFY(sociableActivation > neutralActivation);
    // Boost capped at 15%
    QVERIFY(sociableActivation < neutralActivation * 1.16);
}

void TestMemoryRecallPhase3::testGraphRetrievalIntegration() {
    // Full integration: MemoryStore + graph propagation + ACT-R ranking
    QString storeDbPath = QDir::temp().filePath("test_phase3_store.db");
    QFile::remove(storeDbPath);

    MemoryStore store;
    store.setDatabasePath(storeDbPath);
    QString err;
    QVERIFY(store.loadDatabaseOnly(&err));

    // Create memory chain: ep1 -> ep2 -> ep3
    MemoryEntry ep1;
    ep1.id = "ep1";
    ep1.type = MemoryType::Episodic;
    ep1.partition = "episodic";
    ep1.status = MemoryStatus::Active;
    ep1.summary = "天气讨论，今天晴";
    ep1.tags = {"天气"};
    ep1.importance = 0.6;
    ep1.strength = 0.7;
    store.addEntry(ep1);

    MemoryEntry ep2;
    ep2.id = "ep2";
    ep2.type = MemoryType::Episodic;
    ep2.partition = "episodic";
    ep2.status = MemoryStatus::Active;
    ep2.summary = "户外活动计划，基于天气";
    ep2.tags = {"户外"};
    ep2.importance = 0.5;
    ep2.strength = 0.6;
    store.addEntry(ep2);

    MemoryEntry ep3;
    ep3.id = "ep3";
    ep3.type = MemoryType::Episodic;
    ep3.partition = "episodic";
    ep3.status = MemoryStatus::Active;
    ep3.summary = "活动照片回顾";
    ep3.tags = {"照片"};
    ep3.importance = 0.4;
    ep3.strength = 0.5;
    store.addEntry(ep3);

    // Establish relation chain
    QVERIFY(store.relationGraph().addRelation(makeRelation(
        "g1", "ep1", "ep2", MemoryRelationType::Related, 1.0, 1.0)));
    QVERIFY(store.relationGraph().addRelation(makeRelation(
        "g2", "ep2", "ep3", MemoryRelationType::MentionedWith, 1.0, 1.0)));

    // Setup channels with graph propagation
    ActiveMemoryPool pool;
    pool.activate("ep1", 1.0, "session");

    MemoryKeywordIndex keywordIndex;
    keywordIndex.rebuild(store.all());

    AssociativeActivationEngine engine;
    engine.setRandomSource(fixedRandomSource(0.99));

    ActivationChannels channels;
    channels.activePool = &pool;
    channels.keywordIndex = &keywordIndex;
    channels.graphPropagation = &engine;

    MemoryRetriever retriever;
    MemoryQuery query;
    query.text = "天气";
    query.limit = 8;

    QList<RetrievedMemory> results = retriever.retrieveWithGraphPropagation(store, query, channels);

    // ep1 should rank first (direct match + highest activation)
    QVERIFY(results.size() >= 1);
    QCOMPARE(results[0].entry.id, QString("ep1"));

    // ep2 and ep3 should be discoverable via graph propagation
    QStringList foundIds;
    bool ep2HasGraphChannel = false;
    for (const RetrievedMemory& mem : results) {
        foundIds.append(mem.entry.id);
        if (mem.entry.id == "ep2" &&
            (mem.sourceChannels.contains("graph_propagation") ||
             mem.sourceChannels.contains("graph_exploratory"))) {
            ep2HasGraphChannel = true;
        }
    }
    QVERIFY(foundIds.contains("ep2"));
    QVERIFY(ep2HasGraphChannel);

    QFile::remove(storeDbPath);
}


void TestMemoryRecallPhase3::testExpiredActiveEntriesCannotRecall() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath("expired.db"));
    QVERIFY(store.loadDatabaseOnly());
    MemoryEntry entry;
    entry.id = "expired-active";
    entry.type = MemoryType::Semantic;
    entry.summary = "needle";
    entry.expiresAt = QDateTime::currentDateTimeUtc().addSecs(-1);
    QVERIFY(!store.addEntry(entry).id.isEmpty());
    ActiveMemoryPool pool;
    pool.activate(entry.id, 1.0, "test");
    AssociativeActivationEngine engine;
    engine.setRandomSource(fixedRandomSource(0.99));
    ActivationChannels channels;
    channels.activePool = &pool;
    channels.graphPropagation = &engine;
    MemoryRetriever retriever;
    MemoryQuery query;
    query.text = "needle";
    query.includeInactive = true; // Expiration remains an absolute boundary.
    QVERIFY(retriever.retrieve(store, query).isEmpty());
    QVERIFY(retriever.retrieveActivated(store, query, channels).isEmpty());
    QVERIFY(retriever.retrieveWithGraphPropagation(store, query, channels).isEmpty());
    QCOMPARE(store.readForRecall(entry.id)->status, MemoryStatus::Active);
    QCOMPARE(store.readForRecall(entry.id)->accessCount, 0);
    // A candidate may expire after seed validation; the final read must check again.
    entry.expiresAt = QDateTime::currentDateTimeUtc().addDays(1);
    QVERIFY(store.updateEntryById(entry));
    bool updated = false;
    engine.setRandomSource([&]() {
        entry.expiresAt = QDateTime::currentDateTimeUtc().addSecs(-1);
        updated = store.updateEntryById(entry);
        return 0.99;
    });
    QVERIFY(retriever.retrieveWithGraphPropagation(store, query, channels).isEmpty());
    QVERIFY(updated);
    QCOMPARE(store.readForRecall(entry.id)->accessCount, 0);
}

void TestMemoryRecallPhase3::testForbiddenIntermediate_data() {
    QTest::addColumn<QString>("boundary");
    for (const auto& value : {"deleted", "expired-status", "archived", "sensitive", "expired-time", "missing", "tag", "allowed"})
        QTest::newRow(value) << QString(value);
}

void TestMemoryRecallPhase3::testForbiddenIntermediate() {
    QFETCH(QString, boundary);
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath("boundary.db"));
    QVERIFY(store.loadDatabaseOnly());
    for (const auto& id : {"a", "b", "c"}) {
        MemoryEntry entry;
        entry.id = id;
        entry.type = MemoryType::Semantic;
        entry.summary = id;
        entry.tags = {"allowed"};
        if (entry.id == "b") {
            if (boundary == "missing") continue;
            if (boundary == "deleted") entry.status = MemoryStatus::Deleted;
            if (boundary == "expired-status") entry.status = MemoryStatus::Expired;
            if (boundary == "archived") entry.status = MemoryStatus::Archived;
            if (boundary == "sensitive") entry.privacyLevel = PrivacyLevel::Sensitive;
            if (boundary == "expired-time") entry.expiresAt = QDateTime::currentDateTimeUtc().addSecs(-1);
            if (boundary == "tag") entry.tags.clear();
        }
        QVERIFY(!store.addEntry(entry).id.isEmpty());
    }
    QVERIFY(store.relationGraph().addRelation(makeRelation("ab", "a", "b", MemoryRelationType::Related)));
    QVERIFY(store.relationGraph().addRelation(makeRelation("bc", "b", "c", MemoryRelationType::Related)));
    ActiveMemoryPool pool;
    pool.activate("a", 0.8, "test");
    AssociativeActivationEngine engine;
    engine.setRandomSource(fixedRandomSource(0.99));
    ActivationChannels channels;
    channels.activePool = &pool;
    channels.graphPropagation = &engine;
    MemoryQuery query;
    query.requiredTags = {"allowed"};
    query.limit = 64;
    const auto hits = MemoryRetriever().retrieveWithGraphPropagation(store, query, channels, nullptr, true);
    QStringList ids;
    for (const auto& hit : hits) ids.append(hit.entry.id);
    QCOMPARE(ids.contains("a"), true);
    QCOMPARE(ids.contains("b"), boundary == "allowed");
    QCOMPARE(ids.contains("c"), boundary == "allowed");
}

void TestMemoryRecallPhase3::testNonTraversableEdgesDoNotConsumeBudget() {
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath("neighbors.db"));
    QVERIFY(store.loadDatabaseOnly());
    auto& graph = store.relationGraph();
    // More than the 20-neighbor limit, with higher weights than usable edges.
    for (int i = 0; i < 25; ++i) {
        QVERIFY(graph.addRelation(makeRelation(QString("sup-%1").arg(i), "a",
            QString("s-%1").arg(i), MemoryRelationType::Supersedes)));
        QVERIFY(graph.addRelation(makeRelation(QString("bad-%1").arg(i), "a",
            QString("x-%1").arg(i), MemoryRelationType::Related)));
    }
    QVERIFY(graph.addRelation(makeRelation("ab", "a", "b", MemoryRelationType::MentionedWith, 0.5, 0.9)));
    QVERIFY(graph.addRelation(makeRelation("bc", "b", "c", MemoryRelationType::MentionedWith, 0.5, 0.9)));
    AssociativeActivationEngine engine;
    engine.setRandomSource(fixedRandomSource(0.99));
    const auto hits = engine.propagate({{"a", 0.8}}, graph, nullptr, {},
        [](const QString& id) { return id == "a" || id == "b" || id == "c"; });
    QCOMPARE(hits.size(), 3);
    QHash<QString, double> activation;
    for (const auto& hit : hits) activation[hit.memoryId] = hit.activation;
    const double first = (1.0 / (1.0 + std::exp(-0.8))) * 0.5 * 0.9 * 0.3 * 0.55;
    const double second = (1.0 / (1.0 + std::exp(-first))) * 0.5 * 0.9 * 0.3 * 0.55 * 0.55;
    QVERIFY(qAbs(activation["b"] - first) < 1e-12);
    // Returning to a is filtered before degree counting on the second hop.
    QVERIFY(qAbs(activation["c"] - second) < 1e-12);
}

void TestMemoryRecallPhase3::testDefaultBuilderEdgesPropagate_data() {
    QTest::addColumn<int>("degree");
    QTest::newRow("single") << 1;
    QTest::newRow("neighbor-limit") << 20;
}

void TestMemoryRecallPhase3::testDefaultBuilderEdgesPropagate() {
    QFETCH(int, degree);
    QTemporaryDir directory;
    MemoryStore store;
    store.setDatabasePath(directory.filePath("defaults.db"));
    QVERIFY(store.loadDatabaseOnly());
    HybridGraphBuilder builder(store.relationGraph());
    MemoryEntry seed;
    seed.id = "a";
    seed.type = MemoryType::Episodic;
    seed.summary = "anchor";
    seed.payload["session_id"] = "session";
    for (int i = 0; i < degree; ++i) {
        MemoryEntry neighbor = seed;
        neighbor.id = QString("b-%1").arg(i);
        // Repeated real batches can raise degree above the per-batch cap of 8.
        QCOMPARE(builder.buildForConsolidationBatch({seed, neighbor}), 1);
    }
    const auto edges = store.relationGraph().neighborsOf("a", 20);
    QCOMPARE(edges.size(), degree);
    for (const auto& edge : edges) {
        QCOMPARE(edge.type, MemoryRelationType::MentionedWith);
        QCOMPARE(edge.weight, 0.5);
        QCOMPARE(edge.confidence, 0.9);
    }
    AssociativeActivationEngine engine;
    engine.setRandomSource(fixedRandomSource(0.99));
    const auto hits = engine.propagate({{"a", 0.8}}, store.relationGraph());
    QCOMPARE(hits.size(), degree + 1);
    for (const auto& hit : hits) {
        if (hit.memoryId == "a") continue;
        QVERIFY(hit.activation >= 0.01);
        QCOMPARE(hit.hopCount, 1);
    }
}

void TestMemoryRecallPhase3::testHippocampusScoresSurviveSeedPruning() {
    MemoryStore store;
    class FixedIndex final : public EmbeddingIndex {
    public:
        QList<EmbeddingSearchResult> hits;
        bool upsert(const QString&, const QString&) override { return false; }
        bool remove(const QString&) override { return false; }
        QList<EmbeddingSearchResult> search(const QString&, int limit) override { return hits.mid(0, limit); }
    } index;
    const auto now = QDateTime::currentDateTimeUtc();
    for (int i = 0; i < 20; ++i) {
        MemoryEntry entry;
        entry.id = QString("a-weak-%1").arg(i);
        entry.type = MemoryType::Semantic;
        entry.summary = "jazz";
        QVERIFY(!store.addEntry(entry).id.isEmpty());
        index.hits.append({entry.id, 0.2});
    }
    // Lexical + semantic weak hits used to outrank every hippocampus-only hit.
    MemoryKeywordIndex keywords;
    keywords.rebuild(store.all());
    for (int i = 0; i < 8; ++i) {
        MemoryEntry entry;
        entry.id = QString("z-strong-%1").arg(i);
        entry.type = MemoryType::ShortTerm;
        entry.summary = "jazz music";
        entry.createdAt = now.addDays(-30);
        entry.lastMentionedAt = now;
        QVERIFY(!store.addEntry(entry).id.isEmpty());
    }
    HippocampusWorkingSet workingSet(&store);
    QVERIFY(workingSet.refresh());
    AssociativeActivationEngine engine;
    engine.setRandomSource(fixedRandomSource(0.99));
    ActivationChannels channels;
    channels.embeddingIndex = &index;
    channels.keywordIndex = &keywords;
    channels.workingSet = &workingSet;
    channels.graphPropagation = &engine;
    MemoryQuery query;
    query.text = "jazz music";
    query.limit = 16;
    const auto hits = MemoryRetriever().retrieveWithGraphPropagation(store, query, channels, nullptr, true);
    QCOMPARE(hits.size(), 16);
    int strong = 0;
    for (const auto& hit : hits) if (hit.entry.id.startsWith("z-strong")) ++strong;
    QCOMPARE(strong, 8);
}

QTEST_MAIN(TestMemoryRecallPhase3)
#include "test_memory_recall_phase3.moc"
