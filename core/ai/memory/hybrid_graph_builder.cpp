#include "hybrid_graph_builder.h"

#include <QDateTime>
#include <algorithm>

#include "memory_relation_graph.h"

namespace {

// 事件标识提取（同事件 → MentionedWith）：优先 source_event_id，其次 event_id。
QString eventIdFor(const MemoryEntry& entry) {
    const QString sourceEvent = entry.payload.value(
        QStringLiteral("source_event_id")).toString();
    if (!sourceEvent.isEmpty()) return sourceEvent;
    return entry.payload.value(QStringLiteral("event_id")).toString();
}

} // namespace

HybridGraphBuilder::HybridGraphBuilder(MemoryRelationGraph& graph, Policy policy)
    : m_graph(graph), m_policy(policy) {}

int HybridGraphBuilder::buildForConsolidationBatch(const QList<MemoryEntry>& results) {
    if (results.size() < 2) return 0;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    int edgesCreated = 0;

    // 1. MentionedWith：同情景上下文（session/task/tool）的结果记忆两两相连
    QHash<QString, int> mentionedCountPerNode;
    for (int i = 0; i < results.size(); ++i) {
        for (int j = i + 1; j < results.size(); ++j) {
            const MemoryEntry& a = results.at(i);
            const MemoryEntry& b = results.at(j);
            const QString hintA = contextHintFor(a);
            const QString hintB = contextHintFor(b);
            if (hintA.isEmpty() || hintA != hintB) continue;

            const bool aAtCap = mentionedCountPerNode.value(a.id, 0)
                >= m_policy.mentionedWithMaxPerNode;
            const bool bAtCap = mentionedCountPerNode.value(b.id, 0)
                >= m_policy.mentionedWithMaxPerNode;
            if (aAtCap && bAtCap) continue;

            MemoryRelation relation;
            relation.fromMemoryId = a.id < b.id ? a.id : b.id;
            relation.toMemoryId = a.id < b.id ? b.id : a.id;
            relation.type = MemoryRelationType::MentionedWith;
            relation.weight = 0.5;
            relation.confidence = 0.9;
            relation.provenance = RelationProvenance::Cooccurrence;
            relation.createdAt = now;
            relation.updatedAt = now;
            if (m_graph.addOrReinforceRelation(relation)) {
                ++edgesCreated;
                ++mentionedCountPerNode[a.id];
                ++mentionedCountPerNode[b.id];
            }
        }
    }

    // 2. Related：共享标签的结果记忆两两相连（权重 ∝ 共享标签数）
    for (int i = 0; i < results.size(); ++i) {
        for (int j = i + 1; j < results.size(); ++j) {
            const MemoryEntry& a = results.at(i);
            const MemoryEntry& b = results.at(j);
            if (a.tags.isEmpty() || b.tags.isEmpty()) continue;
            const QSet<QString> tagsA(a.tags.begin(), a.tags.end());
            const QSet<QString> tagsB(b.tags.begin(), b.tags.end());
            const int shared = (tagsA & tagsB).size();
            if (shared <= 0) continue;
            const double weight = qMin(
                m_policy.relatedMaxWeight,
                qMax(m_policy.relatedMinWeight,
                     shared * m_policy.relatedWeightPerSharedTag));
            if (weight < m_policy.relatedMinWeight) continue;

            MemoryRelation relation;
            relation.fromMemoryId = a.id < b.id ? a.id : b.id;
            relation.toMemoryId = a.id < b.id ? b.id : a.id;
            relation.type = MemoryRelationType::Related;
            relation.weight = weight;
            relation.confidence = 0.7;
            relation.provenance = RelationProvenance::Cooccurrence;
            relation.createdAt = now;
            relation.updatedAt = now;
            QJsonObject payload;
            payload.insert(QStringLiteral("shared_tag_count"), shared);
            relation.payload = payload;
            if (m_graph.addOrReinforceRelation(relation)) {
                ++edgesCreated;
            }
        }
    }

    return edgesCreated;
}

int HybridGraphBuilder::buildMentionedWithForEvent(
    const QList<MemoryEntry>& eventMemories) {
    if (eventMemories.size() < 2) return 0;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    int edgesCreated = 0;
    QHash<QString, int> mentionedCountPerNode;

    for (int i = 0; i < eventMemories.size(); ++i) {
        for (int j = i + 1; j < eventMemories.size(); ++j) {
            const MemoryEntry& a = eventMemories.at(i);
            const MemoryEntry& b = eventMemories.at(j);
            const QString eventA = eventIdFor(a);
            const QString eventB = eventIdFor(b);
            if (eventA.isEmpty() || eventA != eventB) continue;

            const bool aAtCap = mentionedCountPerNode.value(a.id, 0)
                >= m_policy.mentionedWithMaxPerNode;
            const bool bAtCap = mentionedCountPerNode.value(b.id, 0)
                >= m_policy.mentionedWithMaxPerNode;
            if (aAtCap && bAtCap) continue;

            MemoryRelation relation;
            relation.fromMemoryId = a.id < b.id ? a.id : b.id;
            relation.toMemoryId = a.id < b.id ? b.id : a.id;
            relation.type = MemoryRelationType::MentionedWith;
            relation.weight = 0.5;
            relation.confidence = 0.9;
            relation.provenance = RelationProvenance::Cooccurrence;
            relation.createdAt = now;
            relation.updatedAt = now;
            QJsonObject payload;
            payload.insert(QStringLiteral("event_id"), eventA);
            relation.payload = payload;
            if (m_graph.addOrReinforceRelation(relation)) {
                ++edgesCreated;
                ++mentionedCountPerNode[a.id];
                ++mentionedCountPerNode[b.id];
            }
        }
    }
    return edgesCreated;
}

HybridGraphBuilder::MaintenanceStats HybridGraphBuilder::maintain(
    const QSet<QString>& validMemoryIds,
    const QStringList& capCheckNodeIds,
    double decayFactor,
    double removeThreshold) {
    MaintenanceStats stats;
    stats.danglingRemoved = m_graph.removeDanglingEdges(validMemoryIds);
    stats.decayRemoved = m_graph.decayAssociativeEdges(decayFactor, removeThreshold);
    for (const QString& nodeId : capCheckNodeIds) {
        stats.capRemoved += m_graph.enforceAssociativeEdgeCap(nodeId);
    }
    return stats;
}

QString HybridGraphBuilder::contextHintFor(const MemoryEntry& entry) {
    const QString sessionId = entry.payload.value(
        QStringLiteral("session_id")).toString();
    if (!sessionId.isEmpty()) return sessionId;
    const QString task = entry.payload.value(QStringLiteral("task")).toString();
    if (!task.isEmpty()) return QStringLiteral("task:") + task;
    const QString tool = entry.payload.value(QStringLiteral("tool")).toString();
    if (!tool.isEmpty()) return QStringLiteral("tool:") + tool;
    return {};
}
