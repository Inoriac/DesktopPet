#ifndef DESKTOP_PET_EMBEDDING_INDEX_H
#define DESKTOP_PET_EMBEDDING_INDEX_H

#include <QList>
#include <QPair>
#include <QString>

struct EmbeddingSearchResult {
    QString memoryId;
    double similarity = 0.0;
};

class EmbeddingIndex {
public:
    virtual ~EmbeddingIndex() = default;

    virtual bool upsert(const QString& memoryId, const QString& text) = 0;
    virtual QList<EmbeddingSearchResult> search(const QString& query, int limit = 10) = 0;
    virtual bool remove(const QString& memoryId) = 0;
};

#endif // DESKTOP_PET_EMBEDDING_INDEX_H
