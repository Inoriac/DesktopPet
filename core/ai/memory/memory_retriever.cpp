#include "memory_retriever.h"

#include <algorithm>
#include <cmath>

#include <QDateTime>
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

    // Phase 6: reinforce retrieved memories (strength + accessCount)
    for (const RetrievedMemory& mem : result) {
        if (!mem.fromGraphExpansion) {
            reinforceMemory(store, mem.entry.id);
        }
    }

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
    return entry.emotionIntensity * 1.5;
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

void MemoryRetriever::reinforceMemory(MemoryStore& store, const QString& id) const {
    MemoryEntry* entry = store.findById(id);
    if (!entry) return;

    entry->strength = qMin(1.0, entry->strength + 0.1);
    entry->accessCount += 1;
    entry->lastAccessedAt = QDateTime::currentDateTimeUtc();
    store.updateEntryById(*entry);
}
