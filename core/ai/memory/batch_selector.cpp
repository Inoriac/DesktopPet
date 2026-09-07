#include "batch_selector.h"

#include <QSet>
#include <QtMath>
#include <algorithm>

#include "memory_store.h"
#include "memory_relation_graph.h"

namespace {

constexpr int kSessionWindowMinutes = 30;  // 同会话窗口：30分钟内
constexpr int kTemporalAdjacentMinutes = 10;  // 时间邻接：10分钟内
constexpr double kMinTextSimilarityThreshold = 0.3;  // 文本相似度门槛

// 提取情绪显著性（直接用 emotionIntensity 字段）
double extractEmotionSalience(const MemoryEntry& entry) {
    return entry.emotionIntensity;
}

// 简单 Jaccard 相似度（用标签 + 词粒度文本）
double computeJaccardSimilarity(const QStringList& tagsA, const QString& textA,
                                 const QStringList& tagsB, const QString& textB) {
    QSet<QString> setA;
    for (const QString& tag : tagsA) setA.insert(tag.toLower());
    const QStringList wordsA = textA.toLower().split(
        QRegularExpression(QStringLiteral("\\W+")), Qt::SkipEmptyParts);
    for (const QString& word : wordsA) {
        if (word.length() >= 3) setA.insert(word);
    }
    
    QSet<QString> setB;
    for (const QString& tag : tagsB) setB.insert(tag.toLower());
    const QStringList wordsB = textB.toLower().split(
        QRegularExpression(QStringLiteral("\\W+")), Qt::SkipEmptyParts);
    for (const QString& word : wordsB) {
        if (word.length() >= 3) setB.insert(word);
    }
    
    if (setA.isEmpty() && setB.isEmpty()) return 0.0;
    const int intersectionSize = (setA & setB).size();
    const int unionSize = (setA | setB).size();
    return unionSize > 0 ? static_cast<double>(intersectionSize) / unionSize : 0.0;
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
    
    // 重要性
    score += entry.importance * m_policy.importanceWeight;
    
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
            || entry.status != MemoryStatus::Active) {
            continue;
        }
        
        // TODO: 检查租约、重试冷却期（需要 MemoryStore 扩展字段）
        
        PriorityCandidate candidate;
        candidate.entry = entry;
        candidate.priorityScore = computePriority(entry, now);
        candidate.waitingHours = entry.createdAt.msecsTo(now) / 3600000.0;
        
        // clusterHint：优先用 payload 中的 session_id，其次 task/tool
        candidate.clusterHint = entry.payload.value(QStringLiteral("session_id")).toString();
        if (candidate.clusterHint.isEmpty()) {
            const QString task = entry.payload.value(QStringLiteral("task")).toString();
            const QString tool = entry.payload.value(QStringLiteral("tool")).toString();
            if (!task.isEmpty()) candidate.clusterHint = QStringLiteral("task:") + task;
            else if (!tool.isEmpty()) candidate.clusterHint = QStringLiteral("tool:") + tool;
        }
        
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
        const QString sessionId = anchor.payload.value(QStringLiteral("session_id")).toString();
        const QString hint = sessionId.isEmpty()
            ? QStringLiteral("orphan")
            : sessionId;
        clusterSizes[hint]++;
    }
    
    // 为每个锚点扩展情景簇（同会话窗口、时间邻接、因果链、标签相似）
    for (const MemoryEntry& anchor : anchors) {
        if (batch.size() >= maxTotal) break;
        
        const QString anchorSession = anchor.payload.value(QStringLiteral("session_id")).toString();
        const QString clusterHint = anchorSession.isEmpty()
            ? QStringLiteral("orphan")
            : anchorSession;
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
            score += textSimilarity(anchor, cand.entry) * 3.0;
            
            if (score >= 5.0) {  // 至少要有一定相关性
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
    const QString sessionA = a.payload.value(QStringLiteral("session_id")).toString();
    const QString sessionB = b.payload.value(QStringLiteral("session_id")).toString();
    if (!sessionA.isEmpty() && sessionA == sessionB) {
        return true;
    }
    const qint64 diffMs = qAbs(a.createdAt.msecsTo(b.createdAt));
    return diffMs <= kSessionWindowMinutes * 60 * 1000;
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
    
    // TODO: 检查图谱中的 CreatedTask / DerivedFrom 边
    
    return false;
}

double BatchSelector::textSimilarity(const MemoryEntry& a,
                                     const MemoryEntry& b) const {
    return computeJaccardSimilarity(a.tags, a.content, b.tags, b.content);
}

int BatchSelector::sharedTagCount(const MemoryEntry& a,
                                  const MemoryEntry& b) const {
    QSet<QString> tagsA = QSet<QString>(a.tags.begin(), a.tags.end());
    QSet<QString> tagsB = QSet<QString>(b.tags.begin(), b.tags.end());
    return (tagsA & tagsB).size();
}
