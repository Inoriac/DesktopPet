#include "memory_retriever.h"

#include <algorithm>
#include <cmath>

#include <QDateTime>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include "memory_store.h"
#include "partition_policy.h"
#include "working_memory_cache.h"
#include "embedding_index.h"

namespace {

bool containsType(const QList<MemoryType>& types, MemoryType type) {
    return std::find(types.cbegin(), types.cend(), type) != types.cend();
}
bool hasAllTags(const QStringList& entryTags, const QStringList& requiredTags) {
    for (const QString& requiredTag : requiredTags) {
        if (!entryTags.contains(requiredTag, Qt::CaseInsensitive)) return false;
    }
    return true;
}

QString joinedSearchText(const MemoryEntry& entry) {
    QString text;
    text += entry.key + QStringLiteral("\n");
    text += entry.summary + QStringLiteral("\n");
    text += entry.content + QStringLiteral("\n");
    text += entry.scope + QStringLiteral("\n");
    text += entry.tags.join(QStringLiteral(" "));
    return text.toLower();
}

QString confidenceLabel(double confidence) {
    if (confidence >= 0.85) return QStringLiteral("高置信度");
    if (confidence >= 0.55) return QStringLiteral("中置信度");
    if (confidence > 0.0) return QStringLiteral("低置信度");
    return QStringLiteral("未知置信度");
}

QString bestSummary(const MemoryEntry& entry) {
    if (!entry.summary.trimmed().isEmpty()) return entry.summary.trimmed();
    if (!entry.content.trimmed().isEmpty()) return entry.content.trimmed();
    return entry.key.trimmed();
}

}

QList<RetrievedMemory> MemoryRetriever::retrieve(
    const QList<MemoryEntry>& entries,
    const MemoryQuery& query,
    const QList<WorkingMemoryItem>& workingMemory,
    const QList<MemoryRelation>& relations) const {
    QList<RetrievedMemory> result;
    const QStringList tokens = tokenize(query.text);
    const int limit = query.limit <= 0 ? 8 : query.limit;

    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (const WorkingMemoryItem& item : workingMemory) {
        if (item.expiresAt.isValid() && item.expiresAt <= now) continue;
        const QString searchText = (item.summary + QLatin1Char(' ') + item.content
                                    + QLatin1Char(' ') + item.tags.join(QLatin1Char(' '))).toLower();
        bool hit = tokens.isEmpty();
        for (const QString& token : tokens) {
            if (searchText.contains(token)) {
                hit = true;
                break;
            }
        }
        if (!hit) continue;

        MemoryEntry synthetic;
        synthetic.id = QStringLiteral("wm:") + item.id;
        synthetic.type = MemoryType::Working;
        synthetic.status = MemoryStatus::Active;
        synthetic.summary = item.summary;
        synthetic.content = item.content;
        synthetic.tags = item.tags;
        synthetic.source = item.source;
        synthetic.importance = item.importance;
        synthetic.createdAt = item.createdAt;
        QStringList reasons{QStringLiteral("working_memory")};
        const double score = scoreEntry(synthetic, query, tokens, &reasons) + 1.5;
        result.append({synthetic, score, reasons, false});
    }

    for (const MemoryEntry& entry : entries) {
        if (!query.includeInactive && entry.status != MemoryStatus::Active) continue;
        if (!query.includeSensitive && entry.privacyLevel == PrivacyLevel::Sensitive) continue;
        if (!query.requiredTags.isEmpty()
            && !hasAllTags(entry.tags, query.requiredTags)) {
            continue;
        }
        QStringList reasons;
        const double score = scoreEntry(entry, query, tokens, &reasons);
        if (score > 0.0) result.append({entry, score, reasons, false});
    }

    std::sort(result.begin(), result.end(), [](const RetrievedMemory& left,
                                               const RetrievedMemory& right) {
        if (std::abs(left.score - right.score) > 0.0001) {
            return left.score > right.score;
        }
        return left.entry.updatedAt > right.entry.updatedAt;
    });

    QHash<QString, MemoryEntry> entriesById;
    for (const MemoryEntry& entry : entries) entriesById.insert(entry.id, entry);
    QSet<QString> seenIds;
    for (const RetrievedMemory& memory : result) seenIds.insert(memory.entry.id);
    const int expansionCount = qMin(3, result.size());
    QList<RetrievedMemory> expanded;
    for (int i = 0; i < expansionCount; ++i) {
        const RetrievedMemory& source = result.at(i);
        QList<MemoryRelation> neighbors;
        for (const MemoryRelation& relation : relations) {
            if (relation.fromMemoryId == source.entry.id
                || relation.toMemoryId == source.entry.id) {
                neighbors.append(relation);
            }
        }
        std::sort(neighbors.begin(), neighbors.end(),
                  [](const MemoryRelation& left, const MemoryRelation& right) {
                      return left.weight > right.weight;
                  });
        while (neighbors.size() > 10) neighbors.removeLast();
        for (const MemoryRelation& relation : neighbors) {
            const QString neighborId = relation.fromMemoryId == source.entry.id
                ? relation.toMemoryId : relation.fromMemoryId;
            if (seenIds.contains(neighborId) || !entriesById.contains(neighborId)) continue;
            const MemoryEntry& neighbor = entriesById[neighborId];
            if (!query.includeInactive && neighbor.status != MemoryStatus::Active) continue;
            if (!query.includeSensitive
                && neighbor.privacyLevel == PrivacyLevel::Sensitive) {
                continue;
            }
            RetrievedMemory memory;
            memory.entry = neighbor;
            memory.score = source.score * relation.weight * 0.5;
            memory.reasons = {QStringLiteral("graph_expansion")};
            memory.fromGraphExpansion = true;
            expanded.append(memory);
            seenIds.insert(neighborId);
        }
    }
    result.append(expanded);
    std::sort(result.begin(), result.end(), [](const RetrievedMemory& left,
                                               const RetrievedMemory& right) {
        if (std::abs(left.score - right.score) > 0.0001) {
            return left.score > right.score;
        }
        return left.entry.updatedAt > right.entry.updatedAt;
    });
    while (result.size() > limit) result.removeLast();
    return result;
}

QList<RetrievedMemory> MemoryRetriever::retrieve(MemoryStore& store,
                                                  const MemoryQuery& query,
                                                  const WorkingMemoryCache* cache,
                                                  EmbeddingIndex* embeddingIndex) const {
    QList<RetrievedMemory> result;
    const QStringList tokens = tokenize(query.text);
    const int limit = query.limit <= 0 ? 8 : query.limit;

    // Phase 0: working memory candidates
    if (cache) {
        const QString queryLower = query.text.toLower();
        for (const WorkingMemoryItem& wm : cache->all()) {
            if (wm.expiresAt.isValid() && wm.expiresAt <= QDateTime::currentDateTimeUtc()) continue;
            const QString searchText = (wm.summary + " " + wm.content + " " + wm.tags.join(" ")).toLower();
            bool hit = false;
            for (const QString& token : tokens) {
                if (searchText.contains(token)) { hit = true; break; }
            }
            if (!hit && !tokens.isEmpty()) continue;

            MemoryEntry synthetic;
            synthetic.id = QStringLiteral("wm:") + wm.id;
            synthetic.type = MemoryType::Working;
            synthetic.status = MemoryStatus::Active;
            synthetic.summary = wm.summary;
            synthetic.content = wm.content;
            synthetic.tags = wm.tags;
            synthetic.source = wm.source;
            synthetic.importance = wm.importance;
            synthetic.createdAt = wm.createdAt;

            QStringList reasons = {QStringLiteral("working_memory")};
            double score = scoreEntry(synthetic, query, tokens, &reasons);
            score += 1.5;

            RetrievedMemory memory;
            memory.entry = synthetic;
            memory.score = score;
            memory.reasons = reasons;
            result.append(memory);
        }
    }

    // Phase 1: score all direct candidates
    for (const MemoryEntry& entry : store.all()) {
        if (!query.includeInactive && entry.status != MemoryStatus::Active) continue;
        if (!query.includeSensitive && entry.privacyLevel == PrivacyLevel::Sensitive) continue;
        if (!query.requiredTags.isEmpty() && !hasAllTags(entry.tags, query.requiredTags)) continue;

        QStringList reasons;
        const double score = scoreEntry(entry, query, tokens, &reasons);
        if (score <= 0.0) continue;

        RetrievedMemory memory;
        memory.entry = entry;
        memory.score = score;
        memory.reasons = reasons;
        result.append(memory);
    }

    // Phase 1.5: embedding candidates (no-op returns empty)
    if (embeddingIndex && !query.text.isEmpty()) {
        QSet<QString> directIds;
        for (const RetrievedMemory& mem : result) {
            directIds.insert(mem.entry.id);
        }
        const QList<EmbeddingSearchResult> embeddingResults = embeddingIndex->search(query.text, limit);
        for (const EmbeddingSearchResult& er : embeddingResults) {
            if (directIds.contains(er.memoryId)) continue;
            const MemoryEntry* entry = store.findById(er.memoryId);
            if (!entry) continue;
            if (!query.includeInactive && entry->status != MemoryStatus::Active) continue;
            if (!query.includeSensitive && entry->privacyLevel == PrivacyLevel::Sensitive) continue;

            RetrievedMemory memory;
            memory.entry = *entry;
            memory.score = er.similarity * 3.0;
            memory.reasons = {QStringLiteral("embedding")};
            result.append(memory);
        }
    }

    // Phase 2: sort to find top candidates for graph expansion
    std::sort(result.begin(), result.end(), [](const RetrievedMemory& a, const RetrievedMemory& b) {
        return a.score > b.score;
    });

    // Phase 3: graph expansion on top 3 candidates
    const MemoryRelationGraph& graph = store.relationGraph();
    QSet<QString> seenIds;
    for (const RetrievedMemory& mem : result) {
        seenIds.insert(mem.entry.id);
    }

    const int expansionCount = qMin(3, result.size());
    QList<RetrievedMemory> expanded;
    for (int i = 0; i < expansionCount; ++i) {
        const RetrievedMemory& source = result[i];
        const QList<MemoryRelation> neighbors = graph.neighborsOf(source.entry.id, 10);

        for (const MemoryRelation& rel : neighbors) {
            const QString neighborId = (rel.fromMemoryId == source.entry.id)
                ? rel.toMemoryId : rel.fromMemoryId;
            if (seenIds.contains(neighborId)) continue;

            const MemoryEntry* neighbor = store.findById(neighborId);
            if (!neighbor) continue;
            if (!query.includeInactive && neighbor->status != MemoryStatus::Active) continue;
            if (!query.includeSensitive && neighbor->privacyLevel == PrivacyLevel::Sensitive) continue;

            RetrievedMemory expandedMem;
            expandedMem.entry = *neighbor;
            expandedMem.score = source.score * rel.weight * 0.5;
            expandedMem.reasons = {QStringLiteral("graph_expansion")};
            expandedMem.fromGraphExpansion = true;
            expanded.append(expandedMem);
            seenIds.insert(neighborId);
        }
    }
    result.append(expanded);

    // Phase 4: final sort
    std::sort(result.begin(), result.end(), [](const RetrievedMemory& a, const RetrievedMemory& b) {
        if (std::abs(a.score - b.score) > 0.0001) return a.score > b.score;
        return a.entry.updatedAt > b.entry.updatedAt;
    });

    // Phase 5: trim to limit
    while (result.size() > limit) {
        result.removeLast();
    }

    // Phase 6: reinforce retrieved memories in one transaction. Committing each
    // hit separately can visibly stall the render loop during message send.
    QStringList reinforcementIds;
    for (const RetrievedMemory& mem : result) {
        if (!mem.fromGraphExpansion) {
            reinforcementIds.append(mem.entry.id);
        }
    }
    store.reinforceEntries(reinforcementIds);

    return result;
}

QStringList MemoryRetriever::formatForContext(const QList<RetrievedMemory>& memories) const {
    QStringList lines;
    int index = 1;
    for (const RetrievedMemory& memory : memories) {
        const MemoryEntry& entry = memory.entry;
        const QString summary = bestSummary(entry);
        if (summary.isEmpty()) continue;

        QStringList labels;
        labels.append(memoryTypeToString(entry.type));
        labels.append(confidenceLabel(entry.confidence));
        if (!entry.scope.trimmed().isEmpty()) {
            labels.append(entry.scope.trimmed());
        }

        lines.append(QStringLiteral("%1. [%2] %3")
                         .arg(index++)
                         .arg(labels.join(QStringLiteral("/")), summary));
    }
    return lines;
}

QStringList MemoryRetriever::tokenize(const QString& text) const {
    QString normalized = text.toLower().trimmed();
    normalized.replace(QRegularExpression(QStringLiteral("[^a-z0-9_\\x{4e00}-\\x{9fa5}]+")), QStringLiteral(" "));

    QStringList tokens;
    const QStringList parts = normalized.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    QSet<QString> seen;
    for (const QString& part : parts) {
        if (part.size() < 2 || seen.contains(part)) continue;
        seen.insert(part);
        tokens.append(part);
    }
    return tokens;
}

double MemoryRetriever::scoreEntry(const MemoryEntry& entry,
                                    const MemoryQuery& query,
                                    const QStringList& tokens,
                                    QStringList* reasons) const {
    double score = 0.0;
    const QString searchText = joinedSearchText(entry);

    // Keyword match
    double keywordScore = 0.0;
    for (const QString& token : tokens) {
        if (searchText.contains(token)) keywordScore += 1.0;
    }
    if (!tokens.isEmpty()) {
        keywordScore /= tokens.size();
        score += keywordScore * 4.0;
        if (keywordScore > 0.0 && reasons) {
            reasons->append(QStringLiteral("keyword"));
        }
    }

    // Type match
    bool typeMatched = false;
    if (!query.preferredTypes.isEmpty() && containsType(query.preferredTypes, entry.type)) {
        score += 2.0;
        typeMatched = true;
        if (reasons) reasons->append(QStringLiteral("type"));
    }

    // Relevance gate: if the query has keywords, require at least keyword or type match
    if (!tokens.isEmpty() && keywordScore <= 0.0 && !typeMatched) {
        return 0.0;
    }

    // Effective strength (with decay)
    const double effectiveStrength = computeEffectiveStrength(entry);
    score += std::clamp(effectiveStrength, 0.0, 1.0) * 1.6;

    // Importance
    score += std::clamp(entry.importance, 0.0, 1.0) * 1.6;

    // Confidence
    score += std::clamp(entry.confidence, 0.0, 1.0) * 1.2;

    // Recency
    const QDateTime reference = entry.lastAccessedAt.isValid() ? entry.lastAccessedAt
                                : entry.updatedAt.isValid()    ? entry.updatedAt
                                                               : entry.createdAt;
    if (reference.isValid()) {
        const qint64 ageDays = std::max<qint64>(0, reference.daysTo(QDateTime::currentDateTimeUtc()));
        const double recency = 1.0 / (1.0 + static_cast<double>(ageDays) / 30.0);
        score += recency * 0.8;
    }

    // Emotion boost
    const double emotionBoost = computeEmotionBoost(entry, query);
    if (emotionBoost > 0.0) {
        score += emotionBoost;
        if (reasons) reasons->append(QStringLiteral("emotion"));
    }

    // Privacy penalty
    if (entry.privacyLevel == PrivacyLevel::Personal) score -= 0.2;
    if (entry.privacyLevel == PrivacyLevel::Sensitive) score -= 4.0;

    // Fallback boost for empty queries
    if (tokens.isEmpty() && query.preferredTypes.isEmpty() && query.requiredTags.isEmpty()) {
        score += 0.5;
    }

    return score;
}

double MemoryRetriever::computeEffectiveStrength(const MemoryEntry& entry) const {
    const QDateTime reference = entry.lastAccessedAt.isValid() ? entry.lastAccessedAt
                                : entry.updatedAt.isValid()    ? entry.updatedAt
                                                               : entry.createdAt;
    if (!reference.isValid()) return entry.strength;

    const qint64 daysSinceAccess = std::max<qint64>(0, reference.daysTo(QDateTime::currentDateTimeUtc()));
    // 自适应衰减：按 partition 取策略，retention(idle) = exp(-idle/eff_half_life)，
    // eff 由 importance(0..1 → 0..10) 与 accessCount 拉伸。不遗忘分区 retention≡1。
    const MemoryPartition p = entry.partition.trimmed().isEmpty()
        ? partitionForType(entry.type)
        : partitionFromString(entry.partition);
    const PartitionDecayPolicy policy = policyFor(p);
    const double retention = policy.retention(entry.importance * 10.0, entry.accessCount,
                                              static_cast<double>(daysSinceAccess));
    return entry.strength * retention;
}

double MemoryRetriever::computeEmotionBoost(const MemoryEntry& entry,
                                             const MemoryQuery& query) const {
    if (query.currentEmotion == EmotionType::Neutral) return 0.0;
    if (entry.emotion == EmotionType::Neutral) return 0.0;
    if (entry.emotion != query.currentEmotion) return 0.0;
    const double memoryIntensity = std::clamp(entry.emotionIntensity, 0.0, 1.0);
    const double currentIntensity = std::clamp(query.currentEmotionIntensity, 0.0, 1.0);
    return std::min(0.35, memoryIntensity * currentIntensity * 0.35);
}

double MemoryRetriever::decayLambda(MemoryType type) const {
    // 已废弃：保留签名以兼容旧测试。衰减现由 computeEffectiveStrength 经
    // PartitionDecayPolicy.retention() 计算（按 partition，importance/access 自适应）。
    switch (type) {
    case MemoryType::Core:
    case MemoryType::Preference:
    case MemoryType::Procedural:
        return 0.01;
    case MemoryType::ShortTerm:
    case MemoryType::Working:
        return 0.3;
    default:
        return 0.05;
    }
}

// ============================================================================
// 类人激活式召回（设计 §1/§6/§7，Phase 2：无图谱传播）
// ============================================================================

#include "active_memory_pool.h"
#include "hippocampus_working_set.h"
#include "memory_keyword_index.h"
#include "memory_cue_extractor.h"

namespace {

constexpr int kActivePoolBudget = 12;
constexpr int kWorkingSetBudget = 8;
constexpr int kEmbeddingBudget = 32;
constexpr int kKeywordBudget = 12;
constexpr int kSeedBudget = 16;

bool passesFilters(const MemoryEntry& entry, const MemoryQuery& query) {
    if (!query.includeInactive && entry.status != MemoryStatus::Active) return false;
    if (!query.includeSensitive && entry.privacyLevel == PrivacyLevel::Sensitive) return false;
    if (!query.requiredTags.isEmpty()) {
        for (const QString& requiredTag : query.requiredTags) {
            if (!entry.tags.contains(requiredTag, Qt::CaseInsensitive)) return false;
        }
    }
    return true;
}

}

QList<RetrievedMemory> MemoryRetriever::retrieveActivated(
    MemoryStore& store,
    const MemoryQuery& query,
    const ActivationChannels& channels,
    MemoryCueExtractor* cueExtractor) const {

    const int limit = query.limit <= 0 ? 8 : query.limit;

    // ---- 阶段 0：时间维护（读取激活池之前必须先衰减）----
    if (channels.activePool) {
        channels.activePool->decayToNow();
    }

    // ---- 阶段 1：本地线索提取（无模型调用）----
    MemoryCue cue;
    if (cueExtractor) {
        cue = cueExtractor->extractFromQuery(query.text);
    } else {
        MemoryCueExtractor fallbackExtractor;
        cue = fallbackExtractor.extractFromQuery(query.text);
    }
    cue.currentEmotion = query.currentEmotion;
    cue.emotionIntensity = query.currentEmotionIntensity;

    // ---- 阶段 2：多路种子（固定预算，某一路不足不强行补齐）----
    // candidateId -> channels
    QHash<QString, QStringList> seedChannels;
    QHash<QString, double> seedRuntimeActivation;

    // 通道 1：近期激活池（最多 12 条）
    if (channels.activePool) {
        const QList<ActiveMemoryItem> activeItems = channels.activePool->activeItems();
        int taken = 0;
        for (const ActiveMemoryItem& item : activeItems) {
            if (taken >= kActivePoolBudget) break;
            seedChannels[item.memoryId].append(QStringLiteral("active_pool"));
            seedRuntimeActivation[item.memoryId] = item.activation;
            ++taken;
        }
    }

    // 通道 2：Hippocampus 工作集扫描（最多 8 条）
    if (channels.workingSet && !channels.workingSet->isEmpty()) {
        const QList<MemoryEntry> scanned = channels.workingSet->scan(
            query.text, query.requiredTags, kWorkingSetBudget);
        for (const MemoryEntry& entry : scanned) {
            if (!seedChannels[entry.id].contains(QLatin1String("hippocampus"))) {
                seedChannels[entry.id].append(QStringLiteral("hippocampus"));
            }
        }
    }

    // 通道 3：Embedding / HNSW（最多 32 条；Noop 索引返回空，自动跳过）
    if (channels.embeddingIndex && !query.text.isEmpty()) {
        const QList<EmbeddingSearchResult> semanticHits =
            channels.embeddingIndex->search(query.text, kEmbeddingBudget);
        for (const EmbeddingSearchResult& hit : semanticHits) {
            if (!seedChannels[hit.memoryId].contains(QLatin1String("embedding"))) {
                seedChannels[hit.memoryId].append(QStringLiteral("embedding"));
            }
        }
    }

    // 通道 4：关键词/标签倒排索引（最多 12 条）
    if (channels.keywordIndex && !channels.keywordIndex->isEmpty()) {
        const QList<QString> keywordHits = channels.keywordIndex->lookup(
            cue.tokens, cue.knownTags, kKeywordBudget);
        for (const QString& memId : keywordHits) {
            if (!seedChannels[memId].contains(QLatin1String("keyword"))) {
                seedChannels[memId].append(QStringLiteral("keyword"));
            }
        }
    }

    // ---- 阶段 3：合并、过滤、构建候选 ----
    QList<CandidateMemory> candidates;
    for (auto it = seedChannels.constBegin(); it != seedChannels.constEnd(); ++it) {
        const MemoryEntry* entry = store.findById(it.key());
        if (!entry) continue;
        if (!passesFilters(*entry, query)) continue;

        CandidateMemory candidate;
        candidate.entry = *entry;
        candidate.sourceChannels = it.value();
        candidate.runtimeActivation = seedRuntimeActivation.value(it.key(), 0.0);
        candidates.append(candidate);
    }

    // 多通道命中优先，超出种子预算时按通道数截断
    if (candidates.size() > kSeedBudget) {
        std::sort(candidates.begin(), candidates.end(),
            [](const CandidateMemory& a, const CandidateMemory& b) {
                if (a.sourceChannels.size() != b.sourceChannels.size()) {
                    return a.sourceChannels.size() > b.sourceChannels.size();
                }
                return a.runtimeActivation > b.runtimeActivation;
            });
        while (candidates.size() > kSeedBudget) candidates.removeLast();
    }

    // ---- 阶段 4：ACT-R 精排 ----
    ACTRRanker ranker;
    const QList<CandidateMemory> ranked = ranker.rank(candidates, cue);

    // ---- 阶段 5：输出与强化（只强化最终进入结果的记忆）----
    QList<RetrievedMemory> result;
    QStringList reinforcementIds;
    for (const CandidateMemory& candidate : ranked) {
        if (result.size() >= limit) break;

        RetrievedMemory memory;
        memory.entry = candidate.entry;
        memory.score = candidate.finalScore;
        memory.reasons = candidate.sourceChannels;
        memory.sourceChannels = candidate.sourceChannels;
        memory.baseActivation = candidate.baseActivation;
        memory.cueMatch = candidate.cueMatch;
        memory.runtimeActivation = candidate.runtimeActivation;
        memory.emotionBoost = candidate.emotionBoost;
        result.append(memory);

        reinforcementIds.append(candidate.entry.id);
    }

    if (!reinforcementIds.isEmpty()) {
        store.reinforceEntries(reinforcementIds);
    }

    return result;
}

// ============================================================================
// Phase 3: 图谱激活传播 + 人格化探索
// ============================================================================

#include "associative_activation_engine.h"

QList<RetrievedMemory> MemoryRetriever::retrieveWithGraphPropagation(
    MemoryStore& store,
    const MemoryQuery& query,
    const ActivationChannels& channels,
    MemoryCueExtractor* cueExtractor) const {

    const int limit = query.limit <= 0 ? 8 : query.limit;

    // ---- 阶段 0：时间维护 ----
    if (channels.activePool) {
        channels.activePool->decayToNow();
    }

    // ---- 阶段 1：本地线索提取 ----
    MemoryCue cue;
    if (cueExtractor) {
        cue = cueExtractor->extractFromQuery(query.text);
    } else {
        MemoryCueExtractor fallbackExtractor;
        cue = fallbackExtractor.extractFromQuery(query.text);
    }
    cue.currentEmotion = query.currentEmotion;
    cue.emotionIntensity = query.currentEmotionIntensity;

    // ---- 阶段 2：多路种子采集（同 Phase 2）----
    QHash<QString, QStringList> seedChannels;
    QHash<QString, double> seedRuntimeActivation;

    // 通道 1: 激活池
    if (channels.activePool) {
        const QList<ActiveMemoryItem> activeItems = channels.activePool->activeItems();
        int taken = 0;
        for (const ActiveMemoryItem& item : activeItems) {
            if (taken >= kActivePoolBudget) break;
            seedChannels[item.memoryId].append(QStringLiteral("active_pool"));
            seedRuntimeActivation[item.memoryId] = item.activation;
            ++taken;
        }
    }

    // 通道 2: Hippocampus 工作集
    if (channels.workingSet && !channels.workingSet->isEmpty()) {
        const QList<MemoryEntry> scanned = channels.workingSet->scan(
            query.text, query.requiredTags, kWorkingSetBudget);
        for (const MemoryEntry& entry : scanned) {
            if (!seedChannels[entry.id].contains(QLatin1String("hippocampus"))) {
                seedChannels[entry.id].append(QStringLiteral("hippocampus"));
            }
        }
    }

    // 通道 3: Embedding
    if (channels.embeddingIndex && !query.text.isEmpty()) {
        const QList<EmbeddingSearchResult> semanticHits =
            channels.embeddingIndex->search(query.text, kEmbeddingBudget);
        for (const EmbeddingSearchResult& hit : semanticHits) {
            if (!seedChannels[hit.memoryId].contains(QLatin1String("embedding"))) {
                seedChannels[hit.memoryId].append(QStringLiteral("embedding"));
            }
        }
    }

    // 通道 4: 关键词倒排
    if (channels.keywordIndex && !channels.keywordIndex->isEmpty()) {
        const QList<QString> keywordHits = channels.keywordIndex->lookup(
            cue.tokens, cue.knownTags, kKeywordBudget);
        for (const QString& memId : keywordHits) {
            if (!seedChannels[memId].contains(QLatin1String("keyword"))) {
                seedChannels[memId].append(QStringLiteral("keyword"));
            }
        }
    }

    // ---- 阶段 3：图谱激活传播（Phase 3 新增）----
    QHash<QString, double> graphActivations;
    QHash<QString, QStringList> graphPaths;

    if (channels.graphPropagation) {
        // Build seed activation map from Phase 2 seeds
        QHash<QString, double> propagationSeeds;
        for (auto it = seedChannels.constBegin(); it != seedChannels.constEnd(); ++it) {
            // Use runtime activation if available, otherwise default 0.8
            const double activation = seedRuntimeActivation.value(it.key(), 0.8);
            propagationSeeds[it.key()] = activation;
        }

        // Propagate (two hops, max 64 candidates)
        const QList<PropagatedMemory> propagated =
            channels.graphPropagation->propagate(
                propagationSeeds,
                store.relationGraph(),
                &store.tagCooccurrenceGraph(),
                cue.knownTags
            );

        // Record graph activation and paths
        for (const PropagatedMemory& prop : propagated) {
            graphActivations[prop.memoryId] = prop.activation;
            graphPaths[prop.memoryId] = prop.propagationPath;

            // Add to seed channels if not already present
            if (!seedChannels.contains(prop.memoryId)) {
                QStringList channels;
                channels.append(prop.isExploratory ? 
                    QStringLiteral("graph_exploratory") : 
                    QStringLiteral("graph_propagation"));
                seedChannels[prop.memoryId] = channels;
            } else if (!seedChannels[prop.memoryId].contains(QLatin1String("graph_propagation"))) {
                seedChannels[prop.memoryId].append(
                    prop.isExploratory ? 
                        QStringLiteral("graph_exploratory") : 
                        QStringLiteral("graph_propagation"));
            }
        }
    }

    // ---- 阶段 4：合并、过滤、构建候选 ----
    QList<CandidateMemory> candidates;
    for (auto it = seedChannels.constBegin(); it != seedChannels.constEnd(); ++it) {
        const MemoryEntry* entry = store.findById(it.key());
        if (!entry) continue;
        if (!passesFilters(*entry, query)) continue;

        CandidateMemory candidate;
        candidate.entry = *entry;
        candidate.sourceChannels = it.value();
        candidate.runtimeActivation = seedRuntimeActivation.value(it.key(), 0.0);
        candidate.graphActivation = graphActivations.value(it.key(), 0.0);
        candidates.append(candidate);
    }

    // 多通道命中优先
    if (candidates.size() > kSeedBudget) {
        std::sort(candidates.begin(), candidates.end(),
            [](const CandidateMemory& a, const CandidateMemory& b) {
                if (a.sourceChannels.size() != b.sourceChannels.size()) {
                    return a.sourceChannels.size() > b.sourceChannels.size();
                }
                return (a.runtimeActivation + a.graphActivation) > 
                       (b.runtimeActivation + b.graphActivation);
            });
        while (candidates.size() > kSeedBudget) candidates.removeLast();
    }

    // ---- 阶段 5：ACT-R 精排（含 G_i 图谱分量）----
    ACTRRanker ranker;
    const QList<CandidateMemory> ranked = ranker.rank(candidates, cue);

    // ---- 阶段 6：输出与强化 ----
    QList<RetrievedMemory> result;
    QStringList reinforcementIds;
    for (const CandidateMemory& candidate : ranked) {
        if (result.size() >= limit) break;

        RetrievedMemory memory;
        memory.entry = candidate.entry;
        memory.score = candidate.finalScore;
        memory.reasons = candidate.sourceChannels;
        memory.sourceChannels = candidate.sourceChannels;
        memory.baseActivation = candidate.baseActivation;
        memory.cueMatch = candidate.cueMatch;
        memory.runtimeActivation = candidate.runtimeActivation;
        memory.emotionBoost = candidate.emotionBoost;
        
        // Add graph propagation path if available
        if (graphPaths.contains(candidate.entry.id)) {
            const QStringList& path = graphPaths[candidate.entry.id];
            if (path.size() > 1) {
                memory.reasons.append(
                    QStringLiteral("graph_path:") + path.join(QStringLiteral("→")));
            }
        }
        
        result.append(memory);
        reinforcementIds.append(candidate.entry.id);
    }

    if (!reinforcementIds.isEmpty()) {
        store.reinforceEntries(reinforcementIds);
    }

    return result;
}
