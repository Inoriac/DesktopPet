#ifndef DESKTOP_PET_MEMORY_RETRIEVER_H
#define DESKTOP_PET_MEMORY_RETRIEVER_H

#include <QList>
#include <QString>
#include <QStringList>

#include "memory_types.h"
#include "memory_relation.h"
#include "actr_ranker.h"

class MemoryStore;
class WorkingMemoryCache;
class EmbeddingIndex;
class ActiveMemoryPool;
class HippocampusWorkingSet;
class MemoryKeywordIndex;
class MemoryCueExtractor;
class AssociativeActivationEngine;
struct WorkingMemoryItem;

struct MemoryQuery {
    QString text;
    QList<MemoryType> preferredTypes;
    QStringList requiredTags;
    int limit = 8;
    bool includeSensitive = false;
    bool includeInactive = false;
    EmotionType currentEmotion = EmotionType::Neutral;
    double currentEmotionIntensity = 0.0;
};

struct RetrievedMemory {
    MemoryEntry entry;
    double score = 0.0;
    QStringList reasons;
    bool fromGraphExpansion = false;
    // 激活式召回的可解释性字段（设计 §13）
    QStringList sourceChannels;       // 候选来源：active_pool/hippocampus/keyword/embedding
    double baseActivation = 0.0;
    double cueMatch = 0.0;
    double runtimeActivation = 0.0;
    double emotionBoost = 0.0;
};

// 激活式召回的可选通道集合。为空的通道自动跳过，不强行补齐。
struct ActivationChannels {
    ActiveMemoryPool* activePool = nullptr;
    HippocampusWorkingSet* workingSet = nullptr;
    const MemoryKeywordIndex* keywordIndex = nullptr;
    EmbeddingIndex* embeddingIndex = nullptr;
    AssociativeActivationEngine* graphPropagation = nullptr;  // Phase 3
};

class MemoryRetriever {
public:
    QList<RetrievedMemory> retrieve(
        const QList<MemoryEntry>& entries,
        const MemoryQuery& query,
        const QList<WorkingMemoryItem>& workingMemory = {},
        const QList<MemoryRelation>& relations = {}) const;

    QList<RetrievedMemory> retrieve(MemoryStore& store,
                                    const MemoryQuery& query,
                                    const WorkingMemoryCache* cache = nullptr,
                                    EmbeddingIndex* embeddingIndex = nullptr) const;

    // 类人激活式召回（设计 §1/§6/§7，Phase 2：无图谱传播）。
    // 固定候选预算：激活池 12 + 工作集 8 + embedding 32 + 关键词/标签 12，
    // 合并去重后最多 16 个种子进 ACT-R 精排，输出最多 query.limit 条。
    // 不扫描 MemoryStore::all()；只强化最终输出的记忆。
    QList<RetrievedMemory> retrieveActivated(MemoryStore& store,
                                             const MemoryQuery& query,
                                             const ActivationChannels& channels,
                                             MemoryCueExtractor* cueExtractor = nullptr) const;

    // Phase 3 完整版：含图谱传播（两跳扩展 + 人格化探索）。
    // 种子预算同上，图谱传播候选预算 64，合并后 ACT-R 精排，输出最多 query.limit。
    // skipReinforcement: 若为 true，跳过自动 store.reinforceEntries()，由调用方自行处理强化。
    QList<RetrievedMemory> retrieveWithGraphPropagation(
        MemoryStore& store,
        const MemoryQuery& query,
        const ActivationChannels& channels,
        MemoryCueExtractor* cueExtractor = nullptr,
        bool skipReinforcement = false) const;

    QStringList formatForContext(const QList<RetrievedMemory>& memories) const;

private:
    QStringList tokenize(const QString& text) const;

    double scoreEntry(const MemoryEntry& entry,
                      const MemoryQuery& query,
                      const QStringList& tokens,
                      QStringList* reasons) const;

    double computeEffectiveStrength(const MemoryEntry& entry) const;
    double computeEmotionBoost(const MemoryEntry& entry,
                               const MemoryQuery& query) const;
    double decayLambda(MemoryType type) const;

};

// Standalone helper for ChatPreparationExecutor Worker thread:
// Creates a temporary MemoryStore, builds activation channels, and performs
// graph propagation recall. Returns RetrievedMemory list + reinforcement IDs.
// The caller (Worker) collects reinforcement IDs and applies them on the main thread.
struct WorkerRecallResult {
    QList<RetrievedMemory> memories;
    QStringList reinforcementIds;
};

WorkerRecallResult retrieveWithGraphPropagationForWorker(
    const QString& databasePath,
    const MemoryQuery& query,
    const QList<WorkingMemoryItem>& workingMemory = {});

#endif // DESKTOP_PET_MEMORY_RETRIEVER_H
