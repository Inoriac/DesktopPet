#ifndef DESKTOP_PET_MEMORY_RETRIEVER_H
#define DESKTOP_PET_MEMORY_RETRIEVER_H

#include <QList>
#include <QString>
#include <QStringList>

#include "memory_types.h"
#include "memory_relation.h"

class MemoryStore;
class WorkingMemoryCache;
class EmbeddingIndex;
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

#endif // DESKTOP_PET_MEMORY_RETRIEVER_H
