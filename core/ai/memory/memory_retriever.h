#ifndef DESKTOP_PET_MEMORY_RETRIEVER_H
#define DESKTOP_PET_MEMORY_RETRIEVER_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QMap>

#include "memory_types.h"

class MemoryStore;
class EmbeddingIndex;
class ActiveMemoryPool;
class HippocampusWorkingSet;
class MemoryKeywordIndex;
class MemoryCueExtractor;
class AssociativeActivationEngine;
struct WorkingMemoryItem;

struct MemoryQuery {
    QString text;
    QStringList requiredTags;
    int limit = 8;
    bool includeSensitive = false;
    bool includeInactive = false;
    EmotionType currentEmotion = EmotionType::Neutral;
    double currentEmotionIntensity = 0.0;
    QString sessionTopic;
    QStringList activeGoals;
    QMap<QString, double> personality;
};

struct RetrievedMemory {
    MemoryEntry entry;
    double score = 0.0;
    QStringList reasons;
    bool fromGraphExpansion = false;
    bool isExploratory = false;
    // 激活式召回的可解释性字段（设计 §13）
    QStringList sourceChannels;       // 候选来源：active_pool/hippocampus/keyword/embedding
    double baseActivation = 0.0;
    double cueMatch = 0.0;
    double lexicalCue = 0.0;
    double tagCue = 0.0;
    double runtimeActivation = 0.0;
    double emotionBoost = 0.0;
    double scoreWithoutEmotion = 0.0; // ACT-R score used for activation feedback
};

// 激活式召回的可选通道集合。为空的通道自动跳过，不强行补齐。
struct ActivationChannels {
    ActiveMemoryPool* activePool = nullptr;
    HippocampusWorkingSet* workingSet = nullptr;
    const MemoryKeywordIndex* keywordIndex = nullptr;
    EmbeddingIndex* embeddingIndex = nullptr;
    AssociativeActivationEngine* graphPropagation = nullptr;
    const QList<WorkingMemoryItem>* workingMemory = nullptr;
};

class MemoryRetriever {
public:
    // 统一召回入口：激活池 12、海马区 8、向量 32、关键词/标签 12；
    // 合并后最多 16 个种子，两跳图传播最多 64 个候选，ACT-R 输出最多 query.limit。
    // 缺失通道自动跳过；workingMemory 可独立运行，无需打开持久化存储。
    // skipReinforcement: 若为 true，跳过自动 store.reinforceEntries()，由调用方自行处理强化。
    QList<RetrievedMemory> retrieveWithGraphPropagation(
        MemoryStore& store,
        const MemoryQuery& query,
        const ActivationChannels& channels,
        MemoryCueExtractor* cueExtractor = nullptr,
        bool skipReinforcement = false) const;

    QStringList formatForContext(const QList<RetrievedMemory>& memories) const;

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

// Reuse a worker-owned store and derived indexes across requests.  The
// database is refreshed by the owner on its own cadence; this entry point
// keeps the chat hot path from rebuilding indexes for every message.
WorkerRecallResult retrieveWithGraphPropagationForWorker(
    MemoryStore& store,
    ActiveMemoryPool& activePool,
    HippocampusWorkingSet& workingSet,
    const MemoryKeywordIndex& keywordIndex,
    EmbeddingIndex* embeddingIndex,
    const MemoryQuery& query,
    const QList<WorkingMemoryItem>& workingMemory = {});

#endif // DESKTOP_PET_MEMORY_RETRIEVER_H
