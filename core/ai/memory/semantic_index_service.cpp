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

void SemanticIndexService::setInterval(int ms) { m_timer.setInterval(ms > 0 ? ms : 1000); }

void SemanticIndexService::kick() {
    if (!isEnabled()) return;
    QTimer::singleShot(0, this, &SemanticIndexService::tick);
}

int SemanticIndexService::runOnce() {
    if (!isEnabled()) return 0;
    if (m_idle && !m_idle()) return 0;

    // 墓碑比例达阈值：空闲期整体重建（权威向量表，不重新推理）。
    if (m_index->isReady() && m_index->needsCompaction() && m_index->activeCount() > 0) {
        QString error;
        if (m_index->rebuildFromRepository(&error)) {
            ++m_rebuildCount;
            emit indexRebuilt();
        } else {
            qWarning() << "[SemanticIndex] compaction rebuild failed:" << error;
        }
    }

    const int completed = m_worker->processPending(m_batchSize);
    if (completed > 0) {
        m_processedTotal += completed;
        emit batchProcessed(completed);
    }
    return completed;
}

void SemanticIndexService::tick() {
    const int completed = runOnce();
    // 一整批都做完 → 大概率还有积压，短延迟继续，空闲期尽快清空队列。
    if (completed >= m_batchSize) QTimer::singleShot(200, this, &SemanticIndexService::tick);
}
