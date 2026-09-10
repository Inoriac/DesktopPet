#ifndef DESKTOP_PET_MEMORY_INDEX_WORKER_H
#define DESKTOP_PET_MEMORY_INDEX_WORKER_H

#include <QString>

class HnswEmbeddingIndex;

// 串行消费 memory_index_jobs outbox。实例应只由一个后台线程调用；不创建线程，
// 这样可复用现有 SleepCycle/后台任务调度，同时保持 SQLite 连接线程归属明确。
class MemoryIndexWorker {
public:
    explicit MemoryIndexWorker(HnswEmbeddingIndex& index);

    // 最多处理 limit 条 Pending/Processing（崩溃恢复）任务，返回成功完成数。
    // 无待处理任务时也尝试加载/恢复索引；limit<=0 不做任何工作。
    int processPending(int limit = 16);
    bool processOne(const QString& jobId);

private:
    bool ensureReady();
    HnswEmbeddingIndex& m_index;
};

#endif // DESKTOP_PET_MEMORY_INDEX_WORKER_H
