#ifndef DESKTOP_PET_SEMANTIC_INDEX_SERVICE_H
#define DESKTOP_PET_SEMANTIC_INDEX_SERVICE_H

#include "embedding_index.h"
#include "embedding_provider.h"
#include "memory_index_worker.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>

class HnswEmbeddingIndex;

// Worker-owned semantic index maintenance. Construct/start/stop on the same
// background thread as the dedicated SQLite connection and provider. QTimer
// serializes maintenance with that worker's recall requests; no GUI work.
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
    int queuedBackfillTotal() const { return m_queuedBackfillTotal; }
    IndexCoverageStats coverageStats(int scanLimit = 4096) const;
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
    int m_queuedBackfillTotal = 0;
    int m_rebuildCount = 0;
};

#endif // DESKTOP_PET_SEMANTIC_INDEX_SERVICE_H
