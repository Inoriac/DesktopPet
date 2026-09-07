#ifndef DESKTOP_PET_BATCH_SELECTOR_H
#define DESKTOP_PET_BATCH_SELECTOR_H

#include <QDateTime>
#include <QList>
#include <QString>

#include "memory_types.h"

class MemoryStore;
class MemoryRelationGraph;

struct BatchSelectionPolicy {
    int minAnchors = 3;
    int maxAnchors = 5;
    int clusterMinSize = 4;
    int clusterMaxSize = 8;
    int batchMaxSize = 20;
    int maxLongTermReferences = 10;
    
    // 优先级权重（可调参）
    double waitingTimeWeight = 1.0;      // 等待时长奖励（小时）
    double importanceWeight = 0.5;       // 重要性（importance 字段）
    double emotionSalienceWeight = 0.3;  // 情绪显著性
    double mentionCountWeight = 0.2;     // 被提及/激活次数
    double goalRelevanceWeight = 0.4;    // 目标相关性（暂未实现）
};

struct PriorityCandidate {
    MemoryEntry entry;
    double priorityScore = 0.0;
    double waitingHours = 0.0;
    QString clusterHint;  // sessionId 或 task/tool 链标识
    
    bool operator<(const PriorityCandidate& other) const {
        return priorityScore < other.priorityScore;
    }
};

// Daydream 批次选择器：优先级评分 + 老化保底 + 锚点选择 + 情景簇扩展
class BatchSelector {
public:
    explicit BatchSelector(MemoryStore& store,
                          MemoryRelationGraph* relationGraph = nullptr,
                          BatchSelectionPolicy policy = {});

    // 从 Hippocampus 中 Pending 候选里选出一批巩固
    QList<MemoryEntry> selectBatch(int maxItems, QList<MemoryEntry>* anchorsOut = nullptr);
    
    // 计算单个候选的巩固优先级
    double computePriority(const MemoryEntry& entry, const QDateTime& now) const;

private:
    QList<PriorityCandidate> gatherCandidates(const QDateTime& now) const;
    QList<MemoryEntry> selectAnchors(const QList<PriorityCandidate>& candidates,
                                     int minCount, int maxCount) const;
    QList<MemoryEntry> expandScenarioClusters(
        const QList<MemoryEntry>& anchors,
        const QList<PriorityCandidate>& remainingCandidates,
        int maxTotal) const;
    
    // 情景簇扩展辅助
    bool sameSessionWindow(const MemoryEntry& a, const MemoryEntry& b) const;
    bool temporallyAdjacent(const MemoryEntry& a, const MemoryEntry& b) const;
    bool sameCausalChain(const MemoryEntry& a, const MemoryEntry& b) const;
    double textSimilarity(const MemoryEntry& a, const MemoryEntry& b) const;
    int sharedTagCount(const MemoryEntry& a, const MemoryEntry& b) const;
    
    MemoryStore& m_store;
    MemoryRelationGraph* m_relationGraph;
    BatchSelectionPolicy m_policy;
};

#endif // DESKTOP_PET_BATCH_SELECTOR_H
