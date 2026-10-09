#include "memory_retriever.h"
#include "actr_ranker.h"

#include <algorithm>
#include <cmath>

#include <QDateTime>
#include <QHash>
#include <QSet>

#include "memory_store.h"
#include "working_memory_cache.h"
#include "embedding_index.h"
#include "active_memory_pool.h"
#include "hippocampus_working_set.h"
#include "memory_keyword_index.h"
#include "memory_cue_extractor.h"
#include "associative_activation_engine.h"

namespace {

bool passesFilters(const MemoryEntry& entry, const MemoryQuery& query,
                   const QDateTime& now) {
    if (entry.status == MemoryStatus::Deleted || entry.status == MemoryStatus::Expired
        || (entry.expiresAt.isValid() && entry.expiresAt <= now)) return false;
    if (!query.includeInactive && entry.status != MemoryStatus::Active) return false;
    if (!query.includeSensitive && entry.privacyLevel == PrivacyLevel::Sensitive) return false;
    if (!query.requiredTags.isEmpty()) {
        for (const QString& requiredTag : query.requiredTags) {
            if (!entry.tags.contains(requiredTag, Qt::CaseInsensitive)) return false;
        }
    }
    return true;
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

QStringList MemoryRetriever::formatForContext(const QList<RetrievedMemory>& memories) const {
    QStringList lines;
    int index = 1;
    for (const RetrievedMemory& memory : memories) {
        const MemoryEntry& entry = memory.entry;
        const QString summary = entry.type == MemoryType::Working && !entry.content.trimmed().isEmpty()
            ? entry.content.trimmed().left(1600) : bestSummary(entry);
        if (summary.isEmpty()) continue;

        QStringList labels;
        labels.append(memoryTypeToString(entry.type));
        labels.append(confidenceLabel(entry.confidence));
        if (entry.type == MemoryType::Working) {
            labels.append(QStringLiteral("临时观察/非用户声明"));
            if (entry.createdAt.isValid()) labels.append(entry.createdAt.toLocalTime().toString(Qt::ISODate));
        }
        if (!entry.scope.trimmed().isEmpty()) {
            labels.append(entry.scope.trimmed());
        }

        lines.append(QStringLiteral("%1. [%2] %3")
                         .arg(index++)
                         .arg(labels.join(QStringLiteral("/")), summary));
    }
    return lines;
}

namespace {
constexpr int kActivePoolBudget = 12;
constexpr int kWorkingSetBudget = 8;
constexpr int kEmbeddingBudget = 32;
constexpr int kKeywordBudget = 12;
constexpr int kSeedBudget = 16;
}

QList<RetrievedMemory> MemoryRetriever::retrieveWithGraphPropagation(
    MemoryStore& store,
    const MemoryQuery& query,
    const ActivationChannels& channels,
    MemoryCueExtractor* cueExtractor,
    bool skipReinforcement) const {

    const int limit = query.limit <= 0 ? 8 : query.limit;
    const QDateTime now = QDateTime::currentDateTimeUtc();

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
    if (channels.keywordIndex) {
        const auto indexedCue = channels.keywordIndex->extractCue(query.text);
        cue.knownTags = indexedCue.knownTags;
        cue.tokenWeights = indexedCue.tokenWeights;
    }
    cue.currentEmotion = query.currentEmotion;
    cue.emotionIntensity = query.currentEmotionIntensity;

    // ---- 阶段 2：多路种子采集----
    QHash<QString, QStringList> seedChannels;
    QHash<QString, double> seedRuntimeActivation;
    QHash<QString, double> seedSemanticCue;

    QHash<QString, double> seedTextCue;
    QHash<QString, double> seedHippocampusScore;

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
        const auto scanned = channels.workingSet->scanScored(
            query.text, query.requiredTags, kWorkingSetBudget);
        for (const auto& candidate : scanned) {
            const auto& entry = candidate.entry;
            seedHippocampusScore[entry.id] = candidate.score;
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
            seedSemanticCue[hit.memoryId] = std::max(seedSemanticCue.value(hit.memoryId, 0.0), hit.similarity);
        }
    }

    // 通道 4: 独立记录词法/概念证据，合并分数不重复累计。
    if (channels.keywordIndex && !channels.keywordIndex->isEmpty()) {
        const auto hits = channels.keywordIndex->lookup(cue, kKeywordBudget);
        for (const auto& hit : hits) {
            if (hit.lexicalScore > 0.0) seedChannels[hit.memoryId].append(QStringLiteral("keyword"));
            if (hit.tagScore > 0.0) seedChannels[hit.memoryId].append(QStringLiteral("tag"));
            seedTextCue[hit.memoryId] = std::max(hit.lexicalScore, hit.tagScore);
        }
    }

    QHash<QString, bool> eligibility;
    const auto mayParticipate = [&](const QString& id) {
        const auto known = eligibility.constFind(id);
        if (known != eligibility.cend()) return known.value();
        const auto entry = store.readForRecall(id);
        const bool allowed = entry && passesFilters(*entry, query, now);
        eligibility.insert(id, allowed);
        return allowed;
    };

    // Validate seeds against SQLite before any propagation. A stale HNSW label
    // must not turn an archived/private source into a bridge to other memories.
    QList<QString> validSeeds;
    for (auto it = seedChannels.constBegin(); it != seedChannels.constEnd(); ++it) {
        if (mayParticipate(it.key())) validSeeds.append(it.key());
    }
    std::sort(validSeeds.begin(), validSeeds.end(), [&](const QString& a, const QString& b) {
        // Compare the strongest normalized channel first. Correlated lexical
        // and tag hits count once; independent channels break equal-score ties.
        const auto strength = [&](const QString& id) {
            const auto unit = [](double score) {
                return std::isfinite(score) ? std::clamp(score, 0.0, 1.0) : 0.0;
            };
            return std::max({unit(seedRuntimeActivation.value(id) / 2.0),
                             unit(seedSemanticCue.value(id)), unit(seedTextCue.value(id)),
                             unit(seedHippocampusScore.value(id))});
        };
        if (strength(a) != strength(b)) return strength(a) > strength(b);
        const auto evidenceCount = [&](const QString& id) {
            const auto channels = seedChannels.value(id);
            return channels.size() - (channels.contains(QStringLiteral("tag"))
                && channels.contains(QStringLiteral("keyword")) ? 1 : 0);
        };
        if (evidenceCount(a) != evidenceCount(b)) return evidenceCount(a) > evidenceCount(b);
        return a < b;
    });
    const QSet<QString> retained(validSeeds.cbegin(), validSeeds.cbegin() + qMin(kSeedBudget, int(validSeeds.size())));
    for (auto it = seedChannels.begin(); it != seedChannels.end();) {
        if (!retained.contains(it.key())) it = seedChannels.erase(it);
        else ++it;
    }

    // ---- 阶段 3：图谱激活传播----
    QHash<QString, double> graphActivations;
    QHash<QString, QStringList> graphPaths;
    QSet<QString> exploratoryIds;

    if (channels.graphPropagation) {
        if (!query.personality.isEmpty()) {
            channels.graphPropagation->setPersonality(
                {cue.openness, cue.sociability, cue.initiative});
        }
        // Build the propagation activation map from retained seeds
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
                cue.knownTags,
                mayParticipate
            );

        // Record graph activation and paths
        for (const PropagatedMemory& prop : propagated) {
            // Seeds are returned for bookkeeping even when no edge reached
            // them. Only actual edge contributions are graph evidence.
            if (prop.activation <= 0.0) continue;
            graphActivations[prop.memoryId] = prop.activation;
            graphPaths[prop.memoryId] = prop.propagationPath;
            if (prop.isExploratory) exploratoryIds.insert(prop.memoryId);

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
        const auto entry = store.readForRecall(it.key());
        if (!entry) continue;
        if (!passesFilters(*entry, query, QDateTime::currentDateTimeUtc())) continue;

        CandidateMemory candidate;
        candidate.entry = *entry;
        candidate.sourceChannels = it.value();
        candidate.runtimeActivation = seedRuntimeActivation.value(it.key(), 0.0);
        candidate.semanticCue = seedSemanticCue.value(it.key(), 0.0);
        candidate.graphActivation = graphActivations.value(it.key(), 0.0);
        candidate.isExploratory = exploratoryIds.contains(it.key());
        candidates.append(candidate);
    }

    // Temporary observations and tool results compete in the same bounded ranking.
    // They never enter SQLite, the persistent activation pool or reinforcement.
    if (channels.workingMemory) {
        const auto now = QDateTime::currentDateTimeUtc();
        int taken = 0;
        for (auto it = channels.workingMemory->crbegin();
             it != channels.workingMemory->crend() && taken < 8; ++it) {
            const WorkingMemoryItem& item = *it;
            if (item.summary.trimmed().isEmpty()
                || (item.expiresAt.isValid() && item.expiresAt <= now)) continue;
            CandidateMemory candidate;
            auto& entry = candidate.entry;
            entry.id = QStringLiteral("wm:") + (item.id.isEmpty()
                ? QString::number(taken) : item.id);
            entry.type = MemoryType::Working;
            entry.status = MemoryStatus::Active;
            entry.privacyLevel = item.privacyLevel;
            entry.summary = item.summary;
            entry.content = item.content;
            entry.tags = item.tags;
            entry.source = item.source;
            entry.importance = item.importance;
            entry.strength = 0.6;
            entry.confidence = item.source == QLatin1String("screen_observation") ? 0.5 : 0.9;
            entry.createdAt = entry.updatedAt = item.createdAt;
            entry.expiresAt = item.expiresAt;
            if (!passesFilters(entry, query, now)) continue;
            candidate.sourceChannels = {QStringLiteral("working_memory")};
            candidate.runtimeActivation = 1.0;
            candidates.append(candidate);
            ++taken;
        }
    }

    // ---- 阶段 5：ACT-R 精排（含 G_i 图谱分量）----
    ACTRRanker ranker;
    const QList<CandidateMemory> ranked = ranker.select(candidates, cue, limit);

    // ---- 阶段 6：输出与强化 ----
    QList<RetrievedMemory> result;
    QStringList reinforcementIds;
    for (const CandidateMemory& candidate : ranked) {
        if (result.size() >= limit) break;

        RetrievedMemory memory;
        memory.entry = candidate.entry;
        memory.score = candidate.finalScore;
        memory.scoreWithoutEmotion = candidate.scoreWithoutEmotion;
        memory.reasons = candidate.sourceChannels;
        memory.sourceChannels = candidate.sourceChannels;
        memory.baseActivation = candidate.baseActivation;
        memory.cueMatch = candidate.cueMatch;
        memory.lexicalCue = candidate.lexicalCue;
        memory.tagCue = candidate.tagCue;
        memory.runtimeActivation = candidate.runtimeActivation;
        memory.emotionBoost = candidate.emotionBoost;
        
        memory.isExploratory = candidate.isExploratory;
        memory.fromGraphExpansion = !retained.contains(candidate.entry.id);

        // Add graph propagation path if available
        if (graphPaths.contains(candidate.entry.id)) {
            const QStringList& path = graphPaths[candidate.entry.id];
            if (path.size() > 1) {
                memory.reasons.append(
                    QStringLiteral("graph_path:") + path.join(QStringLiteral("→")));
            }
        }
        
        result.append(memory);
        if (!candidate.entry.id.startsWith(QLatin1String("wm:"))) {
            reinforcementIds.append(candidate.entry.id);
        }
    }

    if (!skipReinforcement && !reinforcementIds.isEmpty()) {
        store.reinforceEntries(reinforcementIds);
    }

    return result;
}

// ========== Standalone Worker Helper ==========

WorkerRecallResult retrieveWithGraphPropagationForWorker(
    MemoryStore& store,
    ActiveMemoryPool& activePool,
    HippocampusWorkingSet& workingSet,
    const MemoryKeywordIndex& keywordIndex,
    EmbeddingIndex* embeddingIndex,
    const MemoryQuery& query,
    const QList<WorkingMemoryItem>& workingMemory) {
    WorkerRecallResult result;
    AssociativeActivationEngine graphEngine;
    ActivationChannels channels;
    channels.activePool = &activePool;
    channels.workingSet = &workingSet;
    channels.keywordIndex = &keywordIndex;
    channels.embeddingIndex = embeddingIndex;
    channels.graphPropagation = &graphEngine;
    channels.workingMemory = &workingMemory;

    MemoryCueExtractor cueExtractor;
    cueExtractor.setSessionContext(query.sessionTopic, query.activeGoals);
    cueExtractor.setEmotionContext(query.currentEmotion, query.currentEmotionIntensity);
    if (!query.personality.isEmpty()) {
        const auto trait = [&query](const QString& key, double fallback) {
            const double value = query.personality.value(key, fallback);
            return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : fallback;
        };
        cueExtractor.setPersonalityParameters(
            trait(QStringLiteral("openness"), 0.6), trait(QStringLiteral("sociability"), 0.45),
            trait(QStringLiteral("initiative"), 0.35));
    }
    MemoryRetriever retriever;
    const QList<RetrievedMemory> memories = retriever.retrieveWithGraphPropagation(
        store, query, channels, &cueExtractor, /*skipReinforcement=*/true);
    result.memories = memories;
    for (const RetrievedMemory& memory : memories) {
        if (!memory.entry.id.startsWith(QLatin1String("wm:"))
            && !memory.fromGraphExpansion) {
            result.reinforcementIds.append(memory.entry.id);
        }
    }
    return result;
}

WorkerRecallResult retrieveWithGraphPropagationForWorker(
    const QString& databasePath,
    const MemoryQuery& query,
    const QList<WorkingMemoryItem>& workingMemory) {
    
    WorkerRecallResult result;
    
    // 1. Create temporary MemoryStore for the Worker thread
    MemoryStore store;
    store.setDatabasePath(databasePath);
    QString loadError;
    // Chat preparation is a latency-sensitive worker. Load only a bounded
    // recency window (plus a small Hippocampus inbox budget) instead of
    // materializing every historical row on each request.
    if (!store.loadRecallWindow(/*limit=*/256, &loadError)) {
        // Failed to load — return empty result
        return result;
    }
    
    // 2. Build activation channels (lightweight, no persistent state)
    ActiveMemoryPool activePool;  // Empty — no activation history in Worker
    
    HippocampusWorkingSet workingSet(&store);
    workingSet.refresh();  // Load hippocampus entries
    
    MemoryKeywordIndex keywordIndex;
    keywordIndex.rebuild(store.all());  // Bounded recent-window text postings.
    keywordIndex.refreshGlobalTags(store.databaseConnectionName());
    
    // This one-shot helper has no semantic service; the persistent chat Worker does.
    EmbeddingIndex* embeddingIndex = nullptr;
    
    return retrieveWithGraphPropagationForWorker(
        store, activePool, workingSet, keywordIndex, embeddingIndex,
        query, workingMemory);
}
