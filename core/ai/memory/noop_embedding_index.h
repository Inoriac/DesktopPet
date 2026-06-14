#ifndef DESKTOP_PET_NOOP_EMBEDDING_INDEX_H
#define DESKTOP_PET_NOOP_EMBEDDING_INDEX_H

#include "embedding_index.h"
#include "embedding_provider.h"

class NoopEmbeddingProvider : public EmbeddingProvider {
public:
    QString modelName() const override { return QStringLiteral("noop"); }
    int dimension() const override { return 0; }
    QVector<float> embed(const QString&) override { return {}; }
};

class NoopEmbeddingIndex : public EmbeddingIndex {
public:
    bool upsert(const QString&, const QString&) override { return true; }
    QList<EmbeddingSearchResult> search(const QString&, int) override { return {}; }
    bool remove(const QString&) override { return true; }
};

#endif // DESKTOP_PET_NOOP_EMBEDDING_INDEX_H
