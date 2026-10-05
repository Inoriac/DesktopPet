#include "batch_selector.h"

#include <QSet>
#include <QtMath>
#include <algorithm>

#include "memory_store.h"
#include "memory_relation_graph.h"
#include "memory_metadata.h"

namespace {

constexpr int kTemporalAdjacentMinutes = 10;  // 时间邻接：10分钟内
constexpr double kMinTextSimilarity = 0.3;

// 提取情绪显著性（直接用 emotionIntensity 字段）
double extractEmotionSalience(const MemoryEntry& entry) {
    return entry.emotionIntensity;
}

} // namespace

BatchSelector::BatchSelector(MemoryStore& store,
                             MemoryRelationGraph* relationGraph,
                             BatchSelectionPolicy policy)
    : m_store(store),
      m_relationGraph(relationGraph),
      m_policy(policy) {}

QList<MemoryEntry> BatchSelector::selectBatch(int maxItems,
                                               QList<MemoryEntry>* anchorsOut) {
    if (maxItems <= 0) return {};
    
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QList<PriorityCandidate> candidates = gatherCandidates(now);
    
    if (candidates.isEmpty()) return {};
    
    // 1. 选锚点（高分 + 最老保底 + 防簇垄断）
    const QList<MemoryEntry> anchors = selectAnchors(
        candidates, m_policy.minAnchors, m_policy.maxAnchors);
    
    if (anchorsOut) *anchorsOut = anchors;
    
    // 2. 剩余候选
    QSet<QString> anchorIds;
    for (const MemoryEntry& anchor : anchors) anchorIds.insert(anchor.id);
    QList<PriorityCandidate> remaining;
    for (const PriorityCandidate& cand : candidates) {
        if (!anchorIds.contains(cand.entry.id)) {
            remaining.append(cand);
        }
    }
    
    // 3. 情景簇扩展
    QList<MemoryEntry> batch = expandScenarioClusters(
        anchors, remaining, qMin(maxItems, m_policy.batchMaxSize));
    
    return batch;
}

double BatchSelector::computePriority(const MemoryEntry& entry,
                                       const QDateTime& now) const {
    double score = 0.0;
    
    // 等待时长奖励（防止饿死）
    const qint64 waitingMs = entry.createdAt.msecsTo(now);
    const double waitingHours = waitingMs / 3600000.0;
    score += waitingHours * m_policy.waitingTimeWeight;
    
    // Importance is assessed by the model after selection, not an inbox signal.
    
    // 情绪显著性
    const double emotionSalience = extractEmotionSalience(entry);
    score += emotionSalience * m_policy.emotionSalienceWeight;
    
    // 被提及/激活次数
    score += entry.mentionCount * m_policy.mentionCountWeight;
    
    // TODO: 目标相关性（需要 GoalTracker 接入）
    
    return score;
}

QList<PriorityCandidate> BatchSelector::gatherCandidates(const QDateTime& now) const {
    QList<PriorityCandidate> candidates;
    
    for (const MemoryEntry& entry : m_store.all()) {
        // 筛选：Hippocampus + Active 状态（Phase 1 用 Active 表示 Pending）
        if (entry.partition != QLatin1String("hippocampus")
            || entry.status != MemoryStatus::Active
            || entry.type == MemoryType::TaskShadow) {
            continue;
        }
        
        // TODO: 检查租约、重试冷却期（需要 MemoryStore 扩展字段）
        
        PriorityCandidate candidate;
        candidate.entry = entry;
        candidate.priorityScore = computePriority(entry, now);
        candidate.waitingHours = entry.createdAt.msecsTo(now) / 3600000.0;
        
        // clusterHint：优先用 payload 中的 session_id，其次 task/tool
        candidate.clusterHint = MemoryMetadata::contextHint(entry);
        
        candidates.append(candidate);
    }
    
    // 按优先级降序排序
    std::sort(candidates.begin(), candidates.end(),
              [](const PriorityCandidate& a, const PriorityCandidate& b) {
                  return a.priorityScore > b.priorityScore;
              });
    
    return candidates;
}

QList<MemoryEntry> BatchSelector::selectAnchors(
    const QList<PriorityCandidate>& candidates,
    int minCount, int maxCount) const {
    
    if (candidates.isEmpty()) return {};
    
    QList<MemoryEntry> anchors;
    QSet<QString> selectedIds;
    QMap<QString, int> clusterCounts;  // 防簇垄断
    
    // 1. 至少一个来自等待最久的候选（老化保底）
    const PriorityCandidate* oldest = &candidates.first();
    for (const PriorityCandidate& cand : candidates) {
        if (cand.waitingHours > oldest->waitingHours) {
            oldest = &cand;
        }
    }
    anchors.append(oldest->entry);
    selectedIds.insert(oldest->entry.id);
    if (!oldest->clusterHint.isEmpty()) {
        clusterCounts[oldest->clusterHint]++;
    }
    
    // 2. 大部分来自高优先级候选，限制同簇垄断（每簇最多 2 个锚点）
    constexpr int kMaxAnchorsPerCluster = 2;
    for (const PriorityCandidate& cand : candidates) {
        if (anchors.size() >= maxCount) break;
        if (selectedIds.contains(cand.entry.id)) continue;
        
        const int clusterCount = clusterCounts.value(cand.clusterHint, 0);
        if (!cand.clusterHint.isEmpty() && clusterCount >= kMaxAnchorsPerCluster) {
            continue;  // 跳过已垄断簇
        }
        
        anchors.append(cand.entry);
        selectedIds.insert(cand.entry.id);
        if (!cand.clusterHint.isEmpty()) {
            clusterCounts[cand.clusterHint]++;
        }
    }
    
    // 3. 确保至少 minCount 个锚点（降低门槛）
    if (anchors.size() < minCount) {
        for (const PriorityCandidate& cand : candidates) {
            if (anchors.size() >= minCount) break;
            if (!selectedIds.contains(cand.entry.id)) {
                anchors.append(cand.entry);
                selectedIds.insert(cand.entry.id);
            }
        }
    }
    
    return anchors;
}

QList<MemoryEntry> BatchSelector::expandScenarioClusters(
    const QList<MemoryEntry>& anchors,
    const QList<PriorityCandidate>& remainingCandidates,
    int maxTotal) const {
    
    QList<MemoryEntry> batch = anchors;
    QSet<QString> batchIds;
    for (const MemoryEntry& anchor : anchors) {
        batchIds.insert(anchor.id);
    }
    
    QMap<QString, int> clusterSizes;  // 每簇已有记忆数
    for (const MemoryEntry& anchor : anchors) {
        const QString context = MemoryMetadata::contextHint(anchor);
        const QString hint = context.isEmpty() ? QStringLiteral("unassigned:") + anchor.id : context;
        clusterSizes[hint]++;
    }
    
    // 为每个锚点扩展情景簇（同会话窗口、时间邻接、因果链、标签相似）
    for (const MemoryEntry& anchor : anchors) {
        if (batch.size() >= maxTotal) break;
        
        const QString context = MemoryMetadata::contextHint(anchor);
        const QString clusterHint = context.isEmpty() ? QStringLiteral("unassigned:") + anchor.id : context;
        const int currentClusterSize = clusterSizes.value(clusterHint, 0);
        
        if (currentClusterSize >= m_policy.clusterMaxSize) {
            continue;  // 簇已满
        }
        
        // 评分：同会话 > 时间邻接 > 因果链 > 标签相似
        struct ScoredCandidate {
            MemoryEntry entry;
            double score;
            bool operator<(const ScoredCandidate& other) const {
                return score > other.score;  // 降序
            }
        };
        QList<ScoredCandidate> scored;
        
        for (const PriorityCandidate& cand : remainingCandidates) {
            if (batchIds.contains(cand.entry.id)) continue;
            
            double score = 0.0;
            if (sameSessionWindow(anchor, cand.entry)) score += 10.0;
            if (temporallyAdjacent(anchor, cand.entry)) score += 5.0;
            if (sameCausalChain(anchor, cand.entry)) score += 8.0;
            score += sharedTagCount(anchor, cand.entry) * 2.0;
            const double contentSimilarity = textSimilarity(anchor, cand.entry);
            score += contentSimilarity * 3.0;
            
            if (score >= 5.0 || contentSimilarity >= kMinTextSimilarity) {
                scored.append({cand.entry, score});
            }
        }
        
        std::sort(scored.begin(), scored.end());
        
        // 扩展到簇预算上限（4-8 条/簇）
        const int budgetLeft = qMin(
            m_policy.clusterMaxSize - currentClusterSize,
            maxTotal - batch.size());
        
        for (int i = 0; i < budgetLeft && i < scored.size(); ++i) {
            batch.append(scored[i].entry);
            batchIds.insert(scored[i].entry.id);
            clusterSizes[clusterHint]++;
        }
    }
    
    // 簇扩展后若未达批次上限，按优先级填充剩余候选
    if (batch.size() < maxTotal) {
        for (const PriorityCandidate& cand : remainingCandidates) {
            if (batch.size() >= maxTotal) break;
            if (!batchIds.contains(cand.entry.id)) {
                batch.append(cand.entry);
                batchIds.insert(cand.entry.id);
            }
        }
    }
    
    return batch;
}

bool BatchSelector::sameSessionWindow(const MemoryEntry& a,
                                      const MemoryEntry& b) const {
    const auto sessionsA = MemoryMetadata::sessionIds(a);
    const auto sessionsB = MemoryMetadata::sessionIds(b);
    for (const auto& id : sessionsA) if (sessionsB.contains(id)) return true;
    return false; // Unknown or different sessions are not the same context.
}

bool BatchSelector::temporallyAdjacent(const MemoryEntry& a,
                                       const MemoryEntry& b) const {
    const qint64 diffMs = qAbs(a.createdAt.msecsTo(b.createdAt));
    return diffMs <= kTemporalAdjacentMinutes * 60 * 1000;
}

bool BatchSelector::sameCausalChain(const MemoryEntry& a,
                                    const MemoryEntry& b) const {
    const QString taskA = a.payload.value(QStringLiteral("task")).toString();
    const QString taskB = b.payload.value(QStringLiteral("task")).toString();
    if (!taskA.isEmpty() && taskA == taskB) return true;
    
    const QString toolA = a.payload.value(QStringLiteral("tool")).toString();
    const QString toolB = b.payload.value(QStringLiteral("tool")).toString();
    if (!toolA.isEmpty() && toolA == toolB) return true;

    // Structural graph edges carry causal relationships even when entries are
    // far apart in time and have no shared lexical/session cues.  Check both
    // directions because DerivedFrom is commonly stored child -> source while
    // CreatedTask may be stored source -> task.
    if (m_relationGraph) {
        const MemoryRelationType causalTypes[] = {
            MemoryRelationType::CreatedTask,
            MemoryRelationType::DerivedFrom,
        };
        for (const MemoryRelationType type : causalTypes) {
            if (m_relationGraph->hasRelation(a.id, b.id, type)
                || m_relationGraph->hasRelation(b.id, a.id, type)) {
                return true;
            }
        }
    }
    
    return false;
}

double BatchSelector::textSimilarity(const MemoryEntry& a,
                                     const MemoryEntry& b) const {
    return MemoryMetadata::textSimilarity(a, b);
}

int BatchSelector::sharedTagCount(const MemoryEntry& a,
                                  const MemoryEntry& b) const {
    const auto left = MemoryMetadata::semanticTags(a), right = MemoryMetadata::semanticTags(b);
    QSet<QString> tagsA(left.cbegin(), left.cend());
    QSet<QString> tagsB(right.cbegin(), right.cend());
    return (tagsA & tagsB).size();
}
