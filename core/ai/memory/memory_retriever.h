#ifndef DESKTOP_PET_MEMORY_RETRIEVER_H
#define DESKTOP_PET_MEMORY_RETRIEVER_H

#include <QList>
#include <QString>
#include <QStringList>

#include "memory_types.h"

class MemoryStore;

struct MemoryQuery {
    QString text;
    QList<MemoryType> preferredTypes;
    QStringList requiredTags;
    int limit = 8;
    bool includeSensitive = false;
    bool includeInactive = false;
};

struct RetrievedMemory {
    MemoryEntry entry;
    double score = 0.0;
    QStringList reasons;
};

class MemoryRetriever {
public:
    QList<RetrievedMemory> retrieve(const MemoryStore& store,
                                    const MemoryQuery& query) const;
    QStringList formatForContext(const QList<RetrievedMemory>& memories) const;

private:
    QStringList tokenize(const QString& text) const;
    double scoreEntry(const MemoryEntry& entry,
                      const MemoryQuery& query,
                      const QStringList& tokens,
                      QStringList* reasons) const;
};

#endif // DESKTOP_PET_MEMORY_RETRIEVER_H
