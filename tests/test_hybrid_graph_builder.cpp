#include <QtTest>
#include <QJsonObject>
#include <QTemporaryDir>

#include "ai/memory/hybrid_graph_builder.h"
#include "ai/memory/memory_relation_graph.h"
#include "ai/memory/memory_store.h"

namespace {

MemoryEntry makeEntry(const QString& id, const QStringList& tags = {},
                      const QString& sessionId = {}, MemoryType type = MemoryType::Episodic) {
    MemoryEntry entry;
    entry.id = id;
    entry.type = type;
    entry.status = MemoryStatus::Active;
    entry.partition = QStringLiteral("episodic");
    entry.key = id;
    entry.summary = QStringLiteral("entry %1").arg(id);
    entry.content = entry.summary;
    entry.tags = tags;
    entry.createdAt = QDateTime::currentDateTimeUtc();
    entry.updatedAt = entry.createdAt;
    if (!sessionId.isEmpty()) {
        QJsonObject payload;
        payload.insert(QStringLiteral("session_id"), sessionId);
        entry.payload = payload;
    }
    return entry;
}

} // namespace

class TestHybridGraphBuilder : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testBuildForConsolidationBatchMentionedWith();
    void testBuildForConsolidationBatchRelatedBySharedTags();
    void testDifferentContextHintNoMentionedWith();
    void testBuildMentionedWithForEvent();
    void testDuplicateEdgeReinforcement();
    void testDecayAssociativeEdgesStructuralExempt();
    void testRemoveDanglingEdges();
    void testEnforceAssociativeEdgeCap();

private:
    QTemporaryDir m_dir;
    MemoryStore m_store;

    void setupStore();
};

void TestHybridGraphBuilder::initTestCase() {
    QVERIFY(m_dir.isValid());
}

void TestHybridGraphBuilder::cleanupTestCase() {}

void TestHybridGraphBuilder::setupStore() {
    m_store.clear();
    m_store.setStoragePath(m_dir.filePath(QStringLiteral("memory.json")));
    m_store.setDatabasePath(m_dir.filePath(QStringLiteral("memory.db")));
    QVERIFY(m_store.load());
}

void TestHybridGraphBuilder::testBuildForConsolidationBatchMentionedWith() {
    setupStore();
    HybridGraphBuilder builder(m_store.relationGraph());

    // 同 session_id 的两条巩固产出 → MentionedWith
    const QList<MemoryEntry> results = {
        makeEntry(QStringLiteral("mem-a"), {}, QStringLiteral("session-1")),
        makeEntry(QStringLiteral("mem-b"), {}, QStringLiteral("session-1")),
    };
    const int edges = builder.buildForConsolidationBatch(results);
    QVERIFY(edges >= 1);

    const QList<MemoryRelation> relations =
        m_store.relationGraph().neighborsOf(QStringLiteral("mem-a"));
    bool foundMentionedWith = false;
    for (const MemoryRelation& relation : relations) {
        if (relation.type == MemoryRelationType::MentionedWith
            && relation.provenance == RelationProvenance::Cooccurrence) {
            foundMentionedWith = true;
            QCOMPARE(relation.supportCount, 1);
        }
    }
    QVERIFY(foundMentionedWith);
}

void TestHybridGraphBuilder::testBuildForConsolidationBatchRelatedBySharedTags() {
    setupStore();
    HybridGraphBuilder builder(m_store.relationGraph());

    // 共享 2 个标签 → Related，weight = 2 * 0.2 = 0.4
    const QList<MemoryEntry> results = {
        makeEntry(QStringLiteral("mem-a"), {QStringLiteral("tea"), QStringLiteral("morning")}),
        makeEntry(QStringLiteral("mem-b"), {QStringLiteral("tea"), QStringLiteral("morning")}),
    };
    const int edges = builder.buildForConsolidationBatch(results);
    QVERIFY(edges >= 1);

    const QList<MemoryRelation> relations =
        m_store.relationGraph().neighborsOf(QStringLiteral("mem-a"),
                                             MemoryRelationType::Related);
    QCOMPARE(relations.size(), 1);
    QCOMPARE(relations.first().weight, 0.4);
    QCOMPARE(relations.first().provenance, RelationProvenance::Cooccurrence);
    QCOMPARE(relations.first().payload.value(QStringLiteral("shared_tag_count")).toInt(), 2);
}

void TestHybridGraphBuilder::testDifferentContextHintNoMentionedWith() {
    setupStore();
    HybridGraphBuilder builder(m_store.relationGraph());

    // 不同 session 且无共享标签 → 无边
    const QList<MemoryEntry> results = {
        makeEntry(QStringLiteral("mem-a"), {}, QStringLiteral("session-1")),
        makeEntry(QStringLiteral("mem-b"), {}, QStringLiteral("session-2")),
    };
    const int edges = builder.buildForConsolidationBatch(results);
    QCOMPARE(edges, 0);
    QVERIFY(m_store.relationGraph().all().isEmpty());
}

void TestHybridGraphBuilder::testBuildMentionedWithForEvent() {
    setupStore();
    HybridGraphBuilder builder(m_store.relationGraph());

    MemoryEntry a = makeEntry(QStringLiteral("mem-a"));
    MemoryEntry b = makeEntry(QStringLiteral("mem-b"));
    QJsonObject payload;
    payload.insert(QStringLiteral("source_event_id"), QStringLiteral("event-1"));
    a.payload = payload;
    b.payload = payload;

    const int edges = builder.buildMentionedWithForEvent({a, b});
    QCOMPARE(edges, 1);

    const QList<MemoryRelation> relations = m_store.relationGraph().all();
    QCOMPARE(relations.size(), 1);
    QCOMPARE(relations.first().type, MemoryRelationType::MentionedWith);
    QCOMPARE(relations.first().payload.value(QStringLiteral("event_id")).toString(),
             QStringLiteral("event-1"));
}

void TestHybridGraphBuilder::testDuplicateEdgeReinforcement() {
    setupStore();

    MemoryRelation relation;
    relation.fromMemoryId = QStringLiteral("mem-a");
    relation.toMemoryId = QStringLiteral("mem-b");
    relation.type = MemoryRelationType::MentionedWith;
    relation.weight = 0.5;
    relation.provenance = RelationProvenance::Cooccurrence;
    QVERIFY(m_store.relationGraph().addRelation(relation));

    // 第二次同 (from,to,type) → 强化而非新建
    QVERIFY(m_store.relationGraph().addOrReinforceRelation(relation));
    const QList<MemoryRelation> relations = m_store.relationGraph().all();
    QCOMPARE(relations.size(), 1);  // 只有一条边
    QCOMPARE(relations.first().supportCount, 2);
    QVERIFY(relations.first().weight > 0.5);  // weight 叠加
    QVERIFY(relations.first().lastReinforcedAt.isValid());
}

void TestHybridGraphBuilder::testDecayAssociativeEdgesStructuralExempt() {
    setupStore();
    const QDateTime now = QDateTime::currentDateTimeUtc();

    // 联想边：30 天前强化，weight 0.5 → 0.995^30 ≈ 0.43（保留但衰减）
    MemoryRelation associative;
    associative.fromMemoryId = QStringLiteral("mem-a");
    associative.toMemoryId = QStringLiteral("mem-b");
    associative.type = MemoryRelationType::Related;
    associative.weight = 0.5;
    associative.provenance = RelationProvenance::Cooccurrence;
    associative.createdAt = now.addDays(-30);
    associative.updatedAt = now.addDays(-30);
    associative.lastReinforcedAt = now.addDays(-30);
    QVERIFY(m_store.relationGraph().addRelation(associative));

    // 结构性边：同样 30 天前 → 不受衰减影响
    MemoryRelation structural;
    structural.fromMemoryId = QStringLiteral("mem-c");
    structural.toMemoryId = QStringLiteral("mem-d");
    structural.type = MemoryRelationType::DerivedFrom;
    structural.weight = 0.5;
    structural.provenance = RelationProvenance::Rule;
    structural.createdAt = now.addDays(-30);
    structural.updatedAt = now.addDays(-30);
    QVERIFY(m_store.relationGraph().addRelation(structural));

    // 弱联想边：200 天前 → 0.995^200 ≈ 0.37 * 0.2 ≈ 0.074 < 0.15 阈值 → 删除
    MemoryRelation weak;
    weak.fromMemoryId = QStringLiteral("mem-e");
    weak.toMemoryId = QStringLiteral("mem-f");
    weak.type = MemoryRelationType::MentionedWith;
    weak.weight = 0.2;
    weak.provenance = RelationProvenance::Cooccurrence;
    weak.createdAt = now.addDays(-200);
    weak.updatedAt = now.addDays(-200);
    weak.lastReinforcedAt = now.addDays(-200);
    QVERIFY(m_store.relationGraph().addRelation(weak));

    const int removed = m_store.relationGraph().decayAssociativeEdges(0.995, 0.15);
    QCOMPARE(removed, 1);  // 只删弱联想边

    const QList<MemoryRelation> remaining = m_store.relationGraph().all();
    QCOMPARE(remaining.size(), 2);
    for (const MemoryRelation& relation : remaining) {
        if (relation.type == MemoryRelationType::DerivedFrom) {
            QCOMPARE(relation.weight, 0.5);  // 结构性边未衰减
        } else {
            QVERIFY(relation.weight < 0.5);  // 联想边已衰减
            QVERIFY(relation.weight >= 0.15);
        }
    }
}

void TestHybridGraphBuilder::testRemoveDanglingEdges() {
    setupStore();

    MemoryRelation valid;
    valid.fromMemoryId = QStringLiteral("mem-a");
    valid.toMemoryId = QStringLiteral("mem-b");
    valid.type = MemoryRelationType::Related;
    QVERIFY(m_store.relationGraph().addRelation(valid));

    MemoryRelation dangling;
    dangling.fromMemoryId = QStringLiteral("mem-c");
    dangling.toMemoryId = QStringLiteral("ghost");  // 不存在
    dangling.type = MemoryRelationType::Related;
    QVERIFY(m_store.relationGraph().addRelation(dangling));

    const QSet<QString> validIds = {QStringLiteral("mem-a"), QStringLiteral("mem-b")};
    const int removed = m_store.relationGraph().removeDanglingEdges(validIds);
    QCOMPARE(removed, 1);
    QCOMPARE(m_store.relationGraph().all().size(), 1);
}

void TestHybridGraphBuilder::testEnforceAssociativeEdgeCap() {
    setupStore();

    // 节点 mem-hub：35 条联想边 + 2 条结构性边
    for (int i = 0; i < 35; ++i) {
        MemoryRelation relation;
        relation.fromMemoryId = QStringLiteral("mem-hub");
        relation.toMemoryId = QStringLiteral("mem-%1").arg(i);
        relation.type = MemoryRelationType::Related;
        relation.weight = 0.1 + i * 0.02;  // 递增权重
        QVERIFY(m_store.relationGraph().addRelation(relation));
    }
    for (int i = 0; i < 2; ++i) {
        MemoryRelation relation;
        relation.fromMemoryId = QStringLiteral("mem-hub");
        relation.toMemoryId = QStringLiteral("struct-%1").arg(i);
        relation.type = MemoryRelationType::DerivedFrom;
        QVERIFY(m_store.relationGraph().addRelation(relation));
    }

    const int removed = m_store.relationGraph().enforceAssociativeEdgeCap(
        QStringLiteral("mem-hub"), 32);
    QCOMPARE(removed, 3);  // 35 - 32 = 3 条最低权重联想边被删

    // 结构性边完好
    const QList<MemoryRelation> structural = m_store.relationGraph().neighborsOf(
        QStringLiteral("mem-hub"), MemoryRelationType::DerivedFrom);
    QCOMPARE(structural.size(), 2);

    // 联想边恰好 32 条（neighborsOf 默认 limit=20，需显式放大）
    const QList<MemoryRelation> associative = m_store.relationGraph().neighborsOf(
        QStringLiteral("mem-hub"), MemoryRelationType::Related, 100);
    QCOMPARE(associative.size(), 32);
}

QTEST_MAIN(TestHybridGraphBuilder)
#include "test_hybrid_graph_builder.moc"
