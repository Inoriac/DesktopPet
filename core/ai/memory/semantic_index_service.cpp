#include "semantic_index_service.h"

#include "hnsw_embedding_index.h"
#include "memory_index_worker.h"

#include <QDebug>

SemanticIndexService::SemanticIndexService(QObject* parent) : QObject(parent) {
    m_timer.setInterval(30 * 1000);
    connect(&m_timer, &QTimer::timeout, this, &SemanticIndexService::tick);
}

SemanticIndexService::~SemanticIndexService() { stop(); }

bool SemanticIndexService::start(std::unique_ptr<EmbeddingProvider> provider,
                                 const QString& connectionName,
                                 const QString& indexDirectory) {
    stop();
    if (!provider || provider->dimension() <= 0 || connectionName.isEmpty()) {
        qInfo() << "[SemanticIndex] disabled: no embedding provider";
        return false;
    }
    m_provider = std::move(provider);
    m_index = std::make_unique<HnswEmbeddingIndex>(connectionName, m_provider.get(), indexDirectory);
    m_worker = std::make_unique<MemoryIndexWorker>(*m_index);
    m_timer.start();
    kick();  // 启动即尝试加载/恢复索引并消化积压
    qInfo() << "[SemanticIndex] enabled: model=" << m_provider->modelName()
            << "dim=" << m_provider->dimension() << "dir=" << indexDirectory;
    return true;
}

void SemanticIndexService::stop() {
    m_timer.stop();
    m_worker.reset();
    m_index.reset();
    m_provider.reset();
}

EmbeddingIndex* SemanticIndexService::index() const { return m_index.get(); }

IndexCoverageStats SemanticIndexService::coverageStats(int scanLimit) const {
    return m_worker ? m_worker->coverageStats(scanLimit) : IndexCoverageStats{};
}

void SemanticIndexService::setInterval(int ms) { m_timer.setInterval(ms > 0 ? ms : 1000); }

void SemanticIndexService::kick() {
    if (!isEnabled()) return;
    QTimer::singleShot(0, this, &SemanticIndexService::tick);
}

int SemanticIndexService::runOnce() {
    if (!isEnabled()) return 0;
    if (m_idle && !m_idle()) return 0;

    const int queued = m_worker->enqueueBackfillJobs(m_batchSize);
    if (queued > 0) m_queuedBackfillTotal += queued;

    const int completed = m_worker->processPending(m_batchSize);
    if (completed > 0) {
        m_processedTotal += completed;
        emit batchProcessed(completed);
    }

    // 墓碑比例达阈值：空闲期整体重建（权威向量表，不重新推理）。
    // Rebuild even when every active vector has been deleted.  In that case
    // activeCount()==0 while tombstoneRatio()==1.0; retaining the old guard
    // left an all-tombstone index permanently fragmented and prevented the
    // next insertion from reclaiming the deleted labels.
    // Defer compaction until a quiet tick.  A batch that just processed delete
    // jobs must remain observable as tombstones and should not immediately
    // rebuild in the same tick.
    if (completed == 0 && m_index->isReady() && m_index->needsCompaction()) {
        QString error;
        if (m_index->rebuildFromRepository(&error)) {
            ++m_rebuildCount;
            emit indexRebuilt();
        } else {
            m_worker->recordHealthSample(false, error);
            qWarning() << "[SemanticIndex] compaction rebuild failed:" << error;
        }
    }

    m_worker->recordHealthSample(true);
    return completed;
}

void SemanticIndexService::tick() {
    const int completed = runOnce();
    // 一整批都做完 → 大概率还有积压，短延迟继续，空闲期尽快清空队列。
    if (completed >= m_batchSize) QTimer::singleShot(200, this, &SemanticIndexService::tick);
}
