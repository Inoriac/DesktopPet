#include "sqlite_embedding_index.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QCryptographicHash>
#include <QDateTime>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

SqliteEmbeddingIndex::SqliteEmbeddingIndex(QString connectionName, EmbeddingProvider* provider)
    : m_connectionName(std::move(connectionName)), m_provider(provider) {}

SqliteEmbeddingIndex::~SqliteEmbeddingIndex() = default;

QByteArray SqliteEmbeddingIndex::encodeVector(const QVector<float>& v) {
    if (v.isEmpty()) return {};
    return QByteArray(reinterpret_cast<const char*>(v.constData()),
                      static_cast<int>(v.size() * sizeof(float)));
}

QVector<float> SqliteEmbeddingIndex::decodeVector(const QByteArray& blob, int dimension) {
    QVector<float> v;
    if (blob.isEmpty() || dimension <= 0) return v;
    const int expected = dimension * static_cast<int>(sizeof(float));
    if (blob.size() < expected) return v;
    v.resize(dimension);
    std::memcpy(v.data(), blob.constData(), expected);
    return v;
}

double SqliteEmbeddingIndex::cosineSimilarity(const QVector<float>& a, const QVector<float>& b) {
    if (a.isEmpty() || a.size() != b.size()) return 0.0;
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (int i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * b[i];
        na += static_cast<double>(a[i]) * a[i];
        nb += static_cast<double>(b[i]) * b[i];
    }
    na = std::sqrt(na);
    nb = std::sqrt(nb);
    if (na < 1e-12 || nb < 1e-12) return 0.0;
    return dot / (na * nb);
}

QString SqliteEmbeddingIndex::contentHash(const QString& text) {
    return QString::fromUtf8(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha1).toHex());
}

bool SqliteEmbeddingIndex::upsert(const QString& memoryId, const QString& text) {
    if (!m_provider || m_provider->dimension() <= 0) return false;  // 无可用推理后端

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) return false;

    // 内容未变 → 跳过 embed（省推理开销）
    if (m_hashEnabled) {
        const QString hash = contentHash(text);
        QSqlQuery check(db);
        check.prepare(QStringLiteral("SELECT content_hash FROM memory_embeddings WHERE memory_id = :id AND model = :model"));
        check.bindValue(QStringLiteral(":id"), memoryId);
        check.bindValue(QStringLiteral(":model"), m_provider->modelName());
        if (check.exec() && check.next() && check.value(0).toString() == hash) {
            return true;  // 内容一致，无需重算
        }
    }

    const QVector<float> vec = m_provider->embed(text);
    if (vec.isEmpty()) return false;

    QSqlQuery query(db);
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO memory_embeddings"
        " (memory_id, model, dimension, vector_blob, content_hash, updated_at)"
        " VALUES (:id, :model, :dim, :blob, :hash, :ts)"
    ));
    query.bindValue(QStringLiteral(":id"), memoryId);
    query.bindValue(QStringLiteral(":model"), m_provider->modelName());
    query.bindValue(QStringLiteral(":dim"), m_provider->dimension());
    query.bindValue(QStringLiteral(":blob"), encodeVector(vec));
    query.bindValue(QStringLiteral(":hash"), m_hashEnabled ? contentHash(text) : QString());
    query.bindValue(QStringLiteral(":ts"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    return query.exec();
}

bool SqliteEmbeddingIndex::remove(const QString& memoryId) {
    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) return false;
    QSqlQuery query(db);
    query.prepare(QStringLiteral("DELETE FROM memory_embeddings WHERE memory_id = :id"));
    query.bindValue(QStringLiteral(":id"), memoryId);
    return query.exec();
}

QList<EmbeddingSearchResult> SqliteEmbeddingIndex::search(const QString& query, int limit) {
    QList<EmbeddingSearchResult> results;
    if (!m_provider || m_provider->dimension() <= 0 || query.isEmpty()) return results;

    QSqlDatabase db = QSqlDatabase::database(m_connectionName, false);
    if (!db.isOpen()) return results;

    const QVector<float> queryVec = m_provider->embed(query);
    if (queryVec.isEmpty()) return results;

    // 加载同一 model 全部向量（桌面端记忆规模有限，内存内余弦排序可接受；
    // 量大时可改为 SQLite<Scalar> 预筛或 ANN 索引）。
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT memory_id, dimension, vector_blob FROM memory_embeddings WHERE model = :model"
    ));
    q.bindValue(QStringLiteral(":model"), m_provider->modelName());
    if (!q.exec()) return results;

    QList<EmbeddingSearchResult> scored;
    while (q.next()) {
        const QString id = q.value(0).toString();
        const int dim = q.value(1).toInt();
        const QVector<float> vec = decodeVector(q.value(2).toByteArray(), dim);
        if (vec.size() != queryVec.size()) continue;  // 维度不匹配（模型变更），跳过
        EmbeddingSearchResult r;
        r.memoryId = id;
        r.similarity = cosineSimilarity(queryVec, vec);
        scored.append(r);
    }

    std::sort(scored.begin(), scored.end(),
              [](const EmbeddingSearchResult& a, const EmbeddingSearchResult& b) {
                  return a.similarity > b.similarity;
              });
    for (int i = 0; i < std::min<int>(limit, scored.size()); ++i) {
        results.append(scored[i]);
    }
    return results;
}