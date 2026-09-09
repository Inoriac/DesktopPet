#include <QtTest>
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

class TestMemoryRecallPhase3 : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;

private slots:
    void initTestCase();
    void cleanupTestCase();

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
    // Weak edge: low weight + low confidence -> delta below 0.08
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
    QVERIFY(!foundIds.contains("weak_target"));  // Below min delta 0.08
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

QTEST_MAIN(TestMemoryRecallPhase3)
#include "test_memory_recall_phase3.moc"
