#ifndef DESKTOP_PET_EMBEDDING_INDEX_H
#define DESKTOP_PET_EMBEDDING_INDEX_H

#include <QList>
#include <QPair>
#include <QString>
#include <QVector>
#include <cmath>
#include <optional>

struct EmbeddingSearchResult {
    QString memoryId;
    double similarity = 0.0;
};

struct PreparedEmbeddingQuery {
    QVector<float> vector;
    QString model;
    bool attempted = false;
    int encodeCalls = 0;
    qint64 durationMs = 0;
    bool valid() const {
        double norm = 0.0;
        for (float value : vector) {
            if (!std::isfinite(value)) return false;
            norm += double(value) * value;
        }
        return !vector.isEmpty() && std::isfinite(norm) && norm > 1e-12;
    }
};

class EmbeddingIndex {
public:
    virtual ~EmbeddingIndex() = default;

    virtual bool upsert(const QString& memoryId, const QString& text) = 0;
    virtual QList<EmbeddingSearchResult> search(const QString& query, int limit = 10) = 0;
    virtual bool remove(const QString& memoryId) = 0;
    // Optional vector path: an attempted failure must never fall back to another embed.
    virtual PreparedEmbeddingQuery prepareQuery(const QString&) { return {}; }
    virtual QList<EmbeddingSearchResult> searchPrepared(const PreparedEmbeddingQuery& query, int limit) {
        Q_UNUSED(query); Q_UNUSED(limit); return {};
    }
    // Only return evidence for the exact current summary + newline + content.
    virtual std::optional<double> similarityToCurrent(const QString&, const QString&,
                                                      const PreparedEmbeddingQuery&) const { return {}; }

};

#endif // DESKTOP_PET_EMBEDDING_INDEX_H
