#ifndef DESKTOP_PET_DAYDREAM_RELATION_REVIEWER_H
#define DESKTOP_PET_DAYDREAM_RELATION_REVIEWER_H

#include <QList>
#include <QPair>
#include <QSet>
#include <QString>

#include "memory_relation.h"
#include "memory_types.h"

class MemoryRelationGraph;

// 模型复核提案（设计 §10）：模型只能提交包含既有节点、枚举关系、置信度和证据的
// 关系提案，不能自由创建节点或绕过隐私与状态校验。
struct RelationProposal {
    QString fromMemoryId;
    QString toMemoryId;
    MemoryRelationType type = MemoryRelationType::TopicOf;
    double confidence = 0.0;
    QString evidence;
};

// 提案校验（纯函数，设计 §10 约束）：
// - 两端节点必须存在于 validMemoryIds（不能自由创建节点）
// - 类型仅限模型可判断的枚举：TopicOf / ConflictsWith
//   （DerivedFrom/Supersedes/CreatedTask 由代码建立；Related/MentionedWith 由共现建立）
// - confidence ∈ [minConfidence, 1.0]
// - evidence 非空（必须可审计）
// - 不允许自环
// 通过返回空字符串；失败返回原因描述。
QString validateRelationProposal(const RelationProposal& proposal,
                                 const QSet<QString>& validMemoryIds,
                                 double minConfidence = 0.6);

// Daydream 模型复核器（设计 §10）：
// 每个 Daydream 批次最多让模型判断 maxPairsPerBatch（默认 8）对候选。
// 生产流程：候选生成 → 随巩固提示词交给模型判断（零额外 LLM 调用）
// → parseDecisions 解析校验 → applyChangeSet 落库（provenance=Model）。
class DaydreamRelationReviewer {
public:
    struct Policy {
        int maxPairsPerBatch;           // 每批模型判断上限（设计：8）
        double minAcceptedConfidence;   // 落库最低置信度
        int minSharedTagsForCandidate;  // 候选对最少共享标签数
    };

    explicit DaydreamRelationReviewer(Policy policy = {8, 0.6, 1});

    // 候选生成：批次内共享标签的条目对（潜在 TopicOf/ConflictsWith），
    // 按共享标签数降序，最多 maxPairsPerBatch 对。确定性、无模型调用。
    // 供 DaydreamSleepAdapter 拼入巩固提示词的 candidate_relation_pairs。
    QList<QPair<QString, QString>> generateCandidates(
        const QList<MemoryEntry>& entries) const;

    // 应用已校验提案：逐条 validateRelationProposal 后落库。
    // TopicOf：weight=confidence；ConflictsWith：结构性边（不衰减）。
    // provenance=Model，payload 携带 evidence。返回落库边数。
    int applyProposals(const QList<RelationProposal>& proposals,
                       const QList<MemoryEntry>& entries,
                       MemoryRelationGraph& graph);

    Policy policy() const { return m_policy; }

private:
    Policy m_policy;
};

#endif // DESKTOP_PET_DAYDREAM_RELATION_REVIEWER_H
