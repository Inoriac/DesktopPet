#ifndef DESKTOP_PET_SEMANTIC_INDEX_SERVICE_H
#define DESKTOP_PET_SEMANTIC_INDEX_SERVICE_H

#include "embedding_index.h"
#include "embedding_provider.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>

class HnswEmbeddingIndex;
class MemoryIndexWorker;

// 语义索引的生产接线（设计 §5 的简化落地）：
//   持有 EmbeddingProvider + HnswEmbeddingIndex + MemoryIndexWorker，用 QTimer 在
//   空闲 tick 里小批量消费 memory_index_jobs；Daydream 完成后 kick() 立即追加一轮。
//
// 线程模型（刻意简化）：与 MemoryStore 同线程（QSqlDatabase 连接线程绑定），每 tick
// 只处理 batchSize 条任务（bge-small 单次推理毫秒级），且 idlePredicate 为假时跳过，
// 因此不会阻塞 UI。若后续需要独立线程，只需把本对象和一条独立连接移过去。
//
// provider 为空或 dimension()<=0 时服务保持禁用：index() 返回 nullptr，召回退回关键词路径。
class SemanticIndexService : public QObject {
    Q_OBJECT
public:
    explicit SemanticIndexService(QObject* parent = nullptr);
    ~SemanticIndexService() override;

    // 接管 provider 生命周期。成功返回 true 并启动定时器；provider 不可用返回 false（禁用）。
    bool start(std::unique_ptr<EmbeddingProvider> provider,
               const QString& connectionName,
               const QString& indexDirectory);
    void stop();

    bool isEnabled() const { return m_index != nullptr; }
    EmbeddingIndex* index() const;            // 禁用时 nullptr
    HnswEmbeddingIndex* hnswIndex() const { return m_index.get(); }

    // 空闲判定：返回 false 时本 tick 跳过（如正在对话）。默认恒真。
    void setIdlePredicate(std::function<bool()> predicate) { m_idle = std::move(predicate); }
    void setInterval(int ms);                 // 默认 30s
    void setBatchSize(int size) { m_batchSize = size > 0 ? size : 1; } // 默认 4

    // 立即调度一次 tick（Daydream 结束 / 手动整理后调用）。
    void kick();
    // 同步执行一轮（测试 / 显式调用）。返回本轮完成的任务数。
    int runOnce();

    int processedTotal() const { return m_processedTotal; }
    int rebuildCount() const { return m_rebuildCount; }

signals:
    void batchProcessed(int completed);
    void indexRebuilt();

private:
    void tick();

    std::unique_ptr<EmbeddingProvider> m_provider;
    std::unique_ptr<HnswEmbeddingIndex> m_index;
    std::unique_ptr<MemoryIndexWorker> m_worker;
    std::function<bool()> m_idle;
    QTimer m_timer;
    int m_batchSize = 4;
    int m_processedTotal = 0;
    int m_rebuildCount = 0;
};

#endif // DESKTOP_PET_SEMANTIC_INDEX_SERVICE_H
