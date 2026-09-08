#include "daydream_relation_reviewer.h"

#include <QDateTime>
#include <QSet>

#include <algorithm>

#include "memory_relation_graph.h"

namespace {

// 模型可判断的枚举关系（设计 §10）：TopicOf / ConflictsWith。
bool isModelJudgeableType(MemoryRelationType type) {
    return type == MemoryRelationType::TopicOf
        || type == MemoryRelationType::ConflictsWith;
}

} // namespace

DaydreamRelationReviewer::DaydreamRelationReviewer(Policy policy)
    : m_policy(policy) {}

QString validateRelationProposal(const RelationProposal& proposal,
                                 const QSet<QString>& validMemoryIds,
                                 double minConfidence) {
    if (!validMemoryIds.contains(proposal.fromMemoryId)) {
        return QStringLiteral("提案 from 节点不存在（模型不能自由创建节点）");
    }
    if (!validMemoryIds.contains(proposal.toMemoryId)) {
        return QStringLiteral("提案 to 节点不存在（模型不能自由创建节点）");
    }
    if (proposal.fromMemoryId == proposal.toMemoryId) {
        return QStringLiteral("提案不允许自环");
    }
    if (!isModelJudgeableType(proposal.type)) {
        return QStringLiteral("提案关系类型不在模型可判断枚举内（TopicOf/ConflictsWith）");
    }
    if (proposal.confidence < minConfidence || proposal.confidence > 1.0) {
        return QStringLiteral("提案置信度超出接受范围");
    }
    if (proposal.evidence.trimmed().isEmpty()) {
        return QStringLiteral("提案缺少证据（不可审计）");
    }
    return {};
}

QList<QPair<QString, QString>> DaydreamRelationReviewer::generateCandidates(
    const QList<MemoryEntry>& entries) const {
    if (entries.size() < 2) return {};

    struct ScoredPair {
        QString a;
        QString b;
        int sharedTags;
        bool operator<(const ScoredPair& other) const {
            return sharedTags > other.sharedTags;  // 降序
        }
    };
    QList<ScoredPair> scored;
    for (int i = 0; i < entries.size(); ++i) {
        for (int j = i + 1; j < entries.size(); ++j) {
            const MemoryEntry& a = entries.at(i);
            const MemoryEntry& b = entries.at(j);
            if (a.tags.isEmpty() || b.tags.isEmpty()) continue;
            const QSet<QString> tagsA(a.tags.begin(), a.tags.end());
            const QSet<QString> tagsB(b.tags.begin(), b.tags.end());
            const int shared = (tagsA & tagsB).size();
            if (shared < m_policy.minSharedTagsForCandidate) continue;
            scored.append({a.id, b.id, shared});
        }
    }

    std::sort(scored.begin(), scored.end());
    QList<QPair<QString, QString>> candidates;
    const int cap = qMax(1, m_policy.maxPairsPerBatch);
    for (int i = 0; i < scored.size() && candidates.size() < cap; ++i) {
        candidates.append({scored.at(i).a, scored.at(i).b});
    }
    return candidates;
}

int DaydreamRelationReviewer::applyProposals(const QList<RelationProposal>& proposals,
                                             const QList<MemoryEntry>& entries,
                                             MemoryRelationGraph& graph) {
    QSet<QString> validMemoryIds;
    validMemoryIds.reserve(entries.size());
    for (const MemoryEntry& entry : entries) {
        validMemoryIds.insert(entry.id);
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    int applied = 0;
    QSet<QString> appliedPairKeys;
    for (const RelationProposal& proposal : proposals) {
        const QString error = validateRelationProposal(
            proposal, validMemoryIds, m_policy.minAcceptedConfidence);
        if (!error.isEmpty()) continue;  // 不合法提案丢弃（不中断批次）

        // 同批次内同 (from,to,type) 只落库一次（防模型重复提交）
        const QString pairKey = proposal.fromMemoryId + QLatin1Char('|')
            + proposal.toMemoryId + QLatin1Char('|')
            + memoryRelationTypeToString(proposal.type);
        if (appliedPairKeys.contains(pairKey)) continue;
        appliedPairKeys.insert(pairKey);

        MemoryRelation relation;
        relation.fromMemoryId = proposal.fromMemoryId;
        relation.toMemoryId = proposal.toMemoryId;
        relation.type = proposal.type;
        relation.weight = proposal.confidence;
        relation.confidence = proposal.confidence;
        relation.provenance = RelationProvenance::Model;
        relation.createdAt = now;
        relation.updatedAt = now;
        QJsonObject payload;
        payload.insert(QStringLiteral("evidence"), proposal.evidence.left(500));
        relation.payload = payload;
        if (graph.addOrReinforceRelation(relation)) {
            ++applied;
        }
    }
    return applied;
}
