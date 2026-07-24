#ifndef DESKTOP_PET_SQLITE_EMBEDDING_INDEX_H
#define DESKTOP_PET_SQLITE_EMBEDDING_INDEX_H

#include "embedding_index.h"
#include "embedding_provider.h"

#include <QHash>
#include <QString>
#include <QVector>

// 基于 memory_embeddings 表的向量索引实现（复用 SqliteMemoryRepository 的同一 DB 连接）。
//
// 替换 NoopEmbeddingIndex，把语义检索从空壳变成可运行的真实现：
//   - upsert(id, text): 用 EmbeddingProvider 把 text 转向量 → 写 memory_embeddings 表（blob）
//   - search(query, limit): embed(query) → 与库内全部向量算余弦 → top-k
//   - remove(id): 删表行
//
// EmbeddingProvider 可插拔：ONNX 真实推理 / API / 测试用 Fake 均通过同一接口注入。
// 推理后端未就绪时可注入 NoopEmbeddingProvider（dimension=0），index 此时退化为不索引（search 返回空），
// 检索链路其它部分不受影响 —— 与原 Noop 行为等价，但接口已为真模型预留。
//
// vector_blob 编码：单精度浮点小端序连续序列（QVector<float> → QByteArray via toRawData）。
class SqliteEmbeddingIndex : public EmbeddingIndex {
public:
    // connectionName: 与 SqliteMemoryRepository 共用的 QSqlDatabase 连接名。
    // provider: 用于 embed 文本为向量；生命周期由调用方管理，需长于 index。
    SqliteEmbeddingIndex(QString connectionName, EmbeddingProvider* provider);
    ~SqliteEmbeddingIndex() override;

    bool upsert(const QString& memoryId, const QString& text) override;
    QList<EmbeddingSearchResult> search(const QString& query, int limit = 10) override;
    bool remove(const QString& memoryId) override;

    // 内容未变时跳过重新 embed（省推理）。默认开启。
    void setContentHashing(bool enabled) { m_hashEnabled = enabled; }

private:
    QString m_connectionName;
    EmbeddingProvider* m_provider;
    bool m_hashEnabled = true;

    QVector<float> loadVector(const QString& memoryId, int* dimension) const;
    static QByteArray encodeVector(const QVector<float>& v);
    static QVector<float> decodeVector(const QByteArray& blob, int dimension);
    static double cosineSimilarity(const QVector<float>& a, const QVector<float>& b);
    static QString contentHash(const QString& text);
};

#endif // DESKTOP_PET_SQLITE_EMBEDDING_INDEX_H