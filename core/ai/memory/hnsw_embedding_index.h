#ifndef DESKTOP_PET_HNSW_EMBEDDING_INDEX_H
#define DESKTOP_PET_HNSW_EMBEDDING_INDEX_H

#include "embedding_index.h"
#include "embedding_provider.h"

#include <QHash>
#include <QReadWriteLock>
#include <QString>
#include <QVector>

#include <memory>

namespace hnswlib {
template <typename T> class HierarchicalNSW;
class InnerProductSpace;
}

// HNSW 参数（设计 §4，v1.0 初始值）。M/efConstruction 是建图参数（变更触发重建），
// efSearch 是运行时查询参数（变更不触发重建）。
struct HnswIndexParams {
    int m = 16;
    int efConstruction = 200;
    int efSearch = 50;
    double tombstoneRebuildRatio = 0.30;  // 墓碑比例达到即建议重建（由空闲期/Sleep Cycle 执行）
    int initialCapacity = 1024;           // 初始容量，不足时倍增 resize
};

// 基于 hnswlib 的近似最近邻语义索引（设计 §4）。
//
// 角色边界：
//   - SQLite 的 memory_items / memory_embeddings 永远是权威数据；本索引是可丢弃、
//     可从向量表重建的派生结构。
//   - label ↔ memoryId 映射保存在 SQLite 表 memory_hnsw_labels（递增数值 label，
//     避免字符串哈希碰撞）；内容语义变化 = 旧 label 标记删除 + 新 label 插入。
//   - 距离用 InnerProduct + 入库前 L2 归一化 ⇒ 等价 cosine；similarity = 1 - distance。
//
// 线程模型：写操作（upsert/remove/save/rebuild/load）应由单个 MemoryIndexWorker 串行
// 调用；search 可并发（内部 QReadWriteLock 保护）。GUI 线程不得调用 rebuild/save。
//
// 持久化：memory_hnsw_<model-hash8>.bin + 同名 .meta.json 旁路元数据。
// 两个文件分别由 QSaveFile 原子替换，SHA-256 校验检测中途退出造成的文件对不一致。
// 校验失败时 loadFromDisk 返回 false，调用方必须恢复索引后才能继续消费任务。
class HnswEmbeddingIndex : public EmbeddingIndex {
public:
    // connectionName: 与 SqliteMemoryRepository 共用的连接名（label 表 / 向量表）。
    // provider: 文本 → 向量；dimension()<=0 时文本级接口优雅退化（同 Noop）。
    // indexDirectory: 索引文件目录（通常与数据库同目录）。
    HnswEmbeddingIndex(QString connectionName,
                       EmbeddingProvider* provider,
                       QString indexDirectory,
                       HnswIndexParams params = {});
    ~HnswEmbeddingIndex() override;

    // ---- EmbeddingIndex 文本级接口 ----
    bool upsert(const QString& memoryId, const QString& text) override;
    QList<EmbeddingSearchResult> search(const QString& query, int limit = 10) override;
    bool remove(const QString& memoryId) override;

    // ---- 向量级接口（MemoryIndexWorker / 重建复用，不重复 embed）----
    // contentHash 用于幂等：同 memoryId+model+hash 的重复 upsert 不产生重复有效节点。
    bool upsertVector(const QString& memoryId, QVector<float> vector,
                      const QString& contentHash);
    bool removeVector(const QString& memoryId);
    QList<EmbeddingSearchResult> searchVector(const QVector<float>& queryVector,
                                              int limit) const;

    // ---- 生命周期 ----
    // 从磁盘加载并校验元数据 + label 映射一致性；失败返回 false（调用方重建）。
    bool loadFromDisk(QString* errorMessage = nullptr);
    // 两个文件均成功替换后才发布新的 generation。
    bool saveToDisk(QString* errorMessage = nullptr);
    // 权威向量表全量重建：只纳入 Active、非 Sensitive、非 Hippocampus 分区的记忆；
    // 重建在 savepoint 中同步本 model 的 Active label，保留历史 label 分配，消除墓碑。
    // 失败撤销 label 写入并使内存索引不可用，调用方可重试。
    bool rebuildFromRepository(QString* errorMessage = nullptr);

    // ---- 状态观测 ----
    bool isReady() const;
    int activeCount() const;
    int tombstoneCount() const;
    double tombstoneRatio() const;
    bool needsCompaction() const;   // tombstoneRatio >= params.tombstoneRebuildRatio
    qint64 generation() const { return m_generation; }
    QString indexFilePath() const;
    QString metaFilePath() const;
    const HnswIndexParams& params() const { return m_params; }
    QString connectionName() const { return m_connectionName; }
    EmbeddingProvider* provider() const { return m_provider; }

    // 与 SqliteEmbeddingIndex 相同的内容哈希规则（sha1-hex），Worker/生产者共用。
    static QString contentHash(const QString& text);

private:
    bool ensureIndexAllocated(int dimension);
    bool insertVectorLocked(const QString& memoryId, QVector<float>& vector,
                            const QString& hash);
    bool saveToDiskLocked(QString* errorMessage);
    void resetLocked();
    qint64 allocateLabel(const QString& memoryId, const QString& hash);
    bool updateLabelStatus(qint64 label, const QString& status);
    bool loadLabelMapsFromDatabase();
    QString modelFileToken() const;
    static bool normalize(QVector<float>& v);

    QString m_connectionName;
    EmbeddingProvider* m_provider = nullptr;   // non-owning
    QString m_indexDirectory;
    HnswIndexParams m_params;

    mutable QReadWriteLock m_lock;
    std::unique_ptr<hnswlib::InnerProductSpace> m_space;
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> m_index;
    int m_dimension = 0;
    qint64 m_generation = 0;
    int m_tombstones = 0;

    QHash<QString, qint64> m_activeLabelByMemory;   // memoryId -> active label
    QHash<qint64, QString> m_memoryByLabel;         // active label -> memoryId
    QHash<QString, QString> m_hashByMemory;         // memoryId -> content_hash（幂等判断）
};

#endif // DESKTOP_PET_HNSW_EMBEDDING_INDEX_H
