#include <QtTest>
#include <QJsonObject>
#include <QTemporaryDir>

#include "ai/memory/daydream_relation_reviewer.h"
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

    // Phase 4.2.5 模型复核测试
    void testValidateRelationProposalAcceptsValid();
    void testValidateRelationProposalRejectsInvalid();
    void testGenerateCandidatesCapAndOrdering();
    void testApplyProposalsValidatedAndModelProvenance();

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

void TestHybridGraphBuilder::testValidateRelationProposalAcceptsValid() {
    RelationProposal proposal;
    proposal.fromMemoryId = QStringLiteral("mem-a");
    proposal.toMemoryId = QStringLiteral("mem-b");
    proposal.type = MemoryRelationType::TopicOf;
    proposal.confidence = 0.8;
    proposal.evidence = QStringLiteral("两条记忆都围绕同一主题展开");
    const QSet<QString> validIds = {QStringLiteral("mem-a"), QStringLiteral("mem-b")};
    QCOMPARE(validateRelationProposal(proposal, validIds), QString());

    // ConflictsWith 也是模型可判断类型
    proposal.type = MemoryRelationType::ConflictsWith;
    QCOMPARE(validateRelationProposal(proposal, validIds), QString());
}

void TestHybridGraphBuilder::testValidateRelationProposalRejectsInvalid() {
    const QSet<QString> validIds = {QStringLiteral("mem-a"), QStringLiteral("mem-b")};

    RelationProposal proposal;
    proposal.fromMemoryId = QStringLiteral("mem-a");
    proposal.toMemoryId = QStringLiteral("mem-b");
    proposal.type = MemoryRelationType::TopicOf;
    proposal.confidence = 0.8;
    proposal.evidence = QStringLiteral("evidence");

    // 节点不存在（模型不能自由创建节点）
    proposal.toMemoryId = QStringLiteral("ghost");
    QVERIFY(!validateRelationProposal(proposal, validIds).isEmpty());
    proposal.toMemoryId = QStringLiteral("mem-b");

    // 自环
    proposal.toMemoryId = QStringLiteral("mem-a");
    QVERIFY(!validateRelationProposal(proposal, validIds).isEmpty());
    proposal.toMemoryId = QStringLiteral("mem-b");

    // 非模型可判断类型（Related 由共现建立，DerivedFrom 由代码建立）
    proposal.type = MemoryRelationType::Related;
    QVERIFY(!validateRelationProposal(proposal, validIds).isEmpty());
    proposal.type = MemoryRelationType::TopicOf;

    // 置信度低于阈值
    proposal.confidence = 0.3;
    QVERIFY(!validateRelationProposal(proposal, validIds).isEmpty());
    proposal.confidence = 0.8;

    // 证据为空（不可审计）
    proposal.evidence.clear();
    QVERIFY(!validateRelationProposal(proposal, validIds).isEmpty());
}

void TestHybridGraphBuilder::testGenerateCandidatesCapAndOrdering() {
    DaydreamRelationReviewer reviewer;  // 默认策略：每批最多 8 对

    // 10 条记忆，两两共享标签 → 45 对，但候选上限 8
    QList<MemoryEntry> entries;
    for (int i = 0; i < 10; ++i) {
        entries.append(makeEntry(
            QStringLiteral("mem-%1").arg(i),
            {QStringLiteral("shared"), QStringLiteral("tag-%1").arg(i)}));
    }
    const QList<QPair<QString, QString>> candidates =
        reviewer.generateCandidates(entries);
    QCOMPARE(candidates.size(), 8);  // 设计：每批最多 8 对

    // 无共享标签 → 无候选
    const QList<MemoryEntry> noShared = {
        makeEntry(QStringLiteral("mem-x"), {QStringLiteral("a")}),
        makeEntry(QStringLiteral("mem-y"), {QStringLiteral("b")}),
    };
    QVERIFY(reviewer.generateCandidates(noShared).isEmpty());
}

void TestHybridGraphBuilder::testApplyProposalsValidatedAndModelProvenance() {
    setupStore();
    DaydreamRelationReviewer reviewer;
    const QList<MemoryEntry> entries = {
        makeEntry(QStringLiteral("mem-a")),
        makeEntry(QStringLiteral("mem-b")),
        makeEntry(QStringLiteral("mem-c")),
    };

    QList<RelationProposal> proposals;
    // 合法：TopicOf
    RelationProposal good;
    good.fromMemoryId = QStringLiteral("mem-a");
    good.toMemoryId = QStringLiteral("mem-b");
    good.type = MemoryRelationType::TopicOf;
    good.confidence = 0.9;
    good.evidence = QStringLiteral("主题相同");
    proposals.append(good);
    // 合法：ConflictsWith
    RelationProposal conflict;
    conflict.fromMemoryId = QStringLiteral("mem-b");
    conflict.toMemoryId = QStringLiteral("mem-c");
    conflict.type = MemoryRelationType::ConflictsWith;
    conflict.confidence = 0.75;
    conflict.evidence = QStringLiteral("事实矛盾");
    proposals.append(conflict);
    // 非法：引用不存在节点 → 丢弃
    RelationProposal ghost;
    ghost.fromMemoryId = QStringLiteral("mem-a");
    ghost.toMemoryId = QStringLiteral("ghost");
    ghost.type = MemoryRelationType::TopicOf;
    ghost.confidence = 0.9;
    ghost.evidence = QStringLiteral("evidence");
    proposals.append(ghost);
    // 重复：与 good 同对同类型 → 去重
    proposals.append(good);

    const int applied = reviewer.applyProposals(proposals, entries, m_store.relationGraph());
    QCOMPARE(applied, 2);  // 只落库 2 条合法提案

    const QList<MemoryRelation> relations = m_store.relationGraph().all();
    QCOMPARE(relations.size(), 2);
    for (const MemoryRelation& relation : relations) {
        QCOMPARE(relation.provenance, RelationProvenance::Model);
        QCOMPARE(relation.weight, relation.confidence);  // weight = confidence
        QVERIFY(!relation.payload.value(QStringLiteral("evidence")).toString().isEmpty());
        if (relation.type == MemoryRelationType::ConflictsWith) {
            QVERIFY(relation.structural());  // 模型确认的冲突边为结构性
        }
    }
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
